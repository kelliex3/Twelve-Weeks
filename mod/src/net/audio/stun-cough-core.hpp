#pragma once

// Pure core for the stun cough from tools/ext/alert_sting.c (201-270). No engine,
// audio, match or clock calls live here; the owner supplies the liveness gates, the
// clock and the PCM result. The rules kept:
//   * one cough the frame our own stun starts, then one every SC_EVERY_MS (1150)
//     while the lock is live, by the body's gender, random without repeats per
//     gender (ext's t2r_rand xorshift32, prev[2]);
//   * a peer's first stamp coughs at once, later ones at most every 1150 ms, and a
//     stamp older than STUN_SECONDS - 0.3 is dropped (ext's g_t60 - at >= S60(3.2));
//   * the muffled 2-D copy of every wave is built once, with two one-pole low-pass
//     poles (a = 0.889, exp(-2*pi*900/48000)) reset per wave, v = y2 * 1.6 and
//     int16 clamping -- no smear across wave boundaries.
//
// The private PCM table (stun_cough.pcm: 6 male + 6 female one-shots) is not in this
// repo; the guarded sink passes in wave descriptors (off / n / female) and the
// caller-owned output vector. With no data the owner never enters Tick(), so no
// cadence stamp, RNG value or per-slot freshness marker is consumed.
//
// Timing note: ext stores g_stun_pup_at and g_t60 in 1/60 s units; this core takes
// wall-clock milliseconds, so its 3200 ms cap is the exact equivalent of
// S60(3.5f - 0.3f) == 192 frames at 60 Hz. The native owner (stun-cough.cpp) is
// the sole owner of this sub-feature when the ext is absent; boot::ExtLoaded()
// yields it while the ext is loaded. Tests: mod/tests/stun_audio_core_test.cpp.

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace net::audio::stun_cough::core {

    using Sample = int16_t;                 // engine::pcm::Sample

    inline constexpr uint64_t kEveryMs       = 1150;  // ext SC_EVERY_MS
    inline constexpr uint64_t kStunMs        = 3500;  // ext STUN_SECONDS (gameplay::stun_lock)
    inline constexpr uint64_t kElapsedCapMs  = 3200;  // ext S60(STUN_SECONDS - 0.3f)
    inline constexpr float    kMufflePole    = 0.889f;
    inline constexpr float    kMuffleGain    = 1.6f;
    inline constexpr int      kCoreMaxPeers  = 8;     // the mod's largest net slot table

    struct WaveDesc {
        int  off;
        int  n;
        bool female;
    };

    struct Peer {
        uint64_t at     = 0;      // ext g_stun_pup_at[slot] (ms here)
        bool     female = false;
        uint64_t handle = 0;      // puppet handle, for a position fallback
        float    pos[3] = { 0.0f, 0.0f, 0.0f };
    };

    struct State {
        uint64_t ownUntil = 0;    // ext g_stun_until (ms)
        bool     ownFemale = false;
        uint64_t ownNext  = 0;
        bool     ownWas   = false;
        std::array<Peer, kCoreMaxPeers>     peers{};
        std::array<uint64_t, kCoreMaxPeers> seenAt{};   // ext's static pup_at[slot]
        std::array<uint64_t, kCoreMaxPeers> pupNext{};  // ext's static pup_next[slot]
        int      prev[2] = { -1, -1 };
        uint64_t rng = 0;
    };

    // Identical to near-miss-core.hpp's xorshift32 (ext t2r_rand), kept local so
    // the cough core has no dependency on another feature's header.
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

    // ext sc_pick: same gender, never the previous wave for that gender.
    inline int PickWave(State& s, uint64_t now, const WaveDesc* waves,
                        int count, bool female) noexcept
    {
        if (!waves || count <= 0)
            return -1;
        const int gender = female ? 1 : 0;
        int ids[kCoreMaxPeers * 4];
        int n = 0;
        for (int i = 0; i < count && n < static_cast<int>(sizeof ids / sizeof ids[0]); ++i) {
            if (waves[i].female == female && i != s.prev[gender])
                ids[n++] = i;
        }
        if (!n)
            return -1;
        const uint32_t r = NextRand(s.rng, now);
        const int w = ids[static_cast<uint32_t>(r) % static_cast<uint32_t>(n)];
        s.prev[gender] = w;
        return w;
    }

    // ext sc_muffled: one buffer for all waves, but each wave's two poles start at
    // zero. `out` is caller-owned; its size is the highest wave end (ext's `tot`).
    inline void BuildMuffled(std::vector<Sample>& out, const Sample* blob,
                             const WaveDesc* waves, int count)
    {
        int total = 0;
        if (waves) {
            for (int i = 0; i < count; ++i) {
                if (waves[i].off >= 0 && waves[i].n > 0 && waves[i].off < (1 << 24)
                    && waves[i].n < (1 << 24) && waves[i].off + waves[i].n > total)
                    total = waves[i].off + waves[i].n;
            }
        }
        out.assign(total, 0);
        if (!blob || !waves || total <= 0)
            return;

        for (int k = 0; k < count; ++k) {
            const WaveDesc& w = waves[k];
            if (w.off < 0 || w.n <= 0 || w.off + w.n > total)
                continue;
            float y1 = 0.0f, y2 = 0.0f;
            for (int i = w.off; i < w.off + w.n; ++i) {
                y1 = kMufflePole * y1 + (1.0f - kMufflePole) * static_cast<float>(blob[i]);
                y2 = kMufflePole * y2 + (1.0f - kMufflePole) * y1;
                float v = y2 * kMuffleGain;
                if (v > 32767.0f) v = 32767.0f;
                if (v < -32768.0f) v = -32768.0f;
                out[i] = static_cast<Sample>(v);
            }
        }
    }

    inline void NoteOwnStun(State& s, uint64_t startMs) noexcept
    {
        s.ownUntil = startMs + kStunMs;
    }

    inline void NoteOwnStunUntil(State& s, uint64_t untilMs) noexcept
    {
        s.ownUntil = untilMs;
    }

    inline void ClearOwnStun(State& s) noexcept
    {
        s.ownUntil = 0;
        s.ownNext = 0;
        s.ownWas = false;
    }

    inline void SetOwnFemale(State& s, bool female) noexcept { s.ownFemale = female; }

    inline void NotePeerStun(State& s, int slot, uint64_t stunAtMs, bool female,
                             uint64_t handle, const float pos[3] = nullptr) noexcept
    {
        if (slot < 0 || slot >= kCoreMaxPeers)
            return;
        Peer& p = s.peers[slot];
        p.at = stunAtMs;
        p.female = female;
        p.handle = handle;
        p.pos[0] = p.pos[1] = p.pos[2] = 0.0f;
        if (pos)
            for (int j = 0; j < 3; ++j)
                p.pos[j] = pos[j];
    }

    // ext's per-frame "pup_at[s] = 0" invalidation: forget only the tick-side
    // freshness marker / cooldown. The producer's g_stun_pup_at[s] stays, so the
    // same stamp reads fresh again when the gate comes back (exactly like ext).
    inline void ForgetPeer(State& s, int slot) noexcept
    {
        if (slot < 0 || slot >= kCoreMaxPeers)
            return;
        s.seenAt[slot] = 0;
        s.pupNext[slot] = 0;
    }

    // Full producer clear, for a peer that respawned / dropped / had its stun cut.
    inline void ClearPeer(State& s, int slot) noexcept
    {
        if (slot < 0 || slot >= kCoreMaxPeers)
            return;
        s.peers[slot] = Peer{};
        ForgetPeer(s, slot);
    }

    struct PeerCue {
        int      slot = -1;
        int      wave = -1;
        uint64_t handle = 0;
        float    pos[3] = { 0.0f, 0.0f, 0.0f };
        bool     hasPos = false;
        bool     fresh = false;   // log-only: this stamp was new, not a cadence repeat
    };

    struct Result {
        bool own = false;         // play the muffled own wave at 0.9 / 0.9
        int  ownWave = -1;
        std::array<PeerCue, kCoreMaxPeers> peers{};
        int  peerCount = 0;
    };

    // `localOk` is the ext's !downed && !down_dying && !uc4_dying && !psync_active()
    // && local_hp > 0; `peerOk[slot]` is the ext's !g_peer_downed && !g_puppet_dead_here
    // (plus the native puppet-alive gate). The caller supplies them so no engine
    // state is read here.
    inline Result Tick(State& s, uint64_t now, bool matchLive, bool localOk,
                       const bool* peerOk, int peerCount,
                       const WaveDesc* waves, int waveCount) noexcept
    {
        Result out;
        const bool own = matchLive && s.ownUntil != 0 && now < s.ownUntil && localOk;
        if (own && (!s.ownWas || now >= s.ownNext)) {
            out.ownWave = PickWave(s, now, waves, waveCount, s.ownFemale);
            out.own = out.ownWave >= 0;
            s.ownNext = now + kEveryMs;   // ext stamps even when the play fails
        }
        s.ownWas = own;

        if (!matchLive) {
            for (int i = 0; i < kCoreMaxPeers; ++i)
                ForgetPeer(s, i);
            return out;
        }

        const int n = peerCount < kCoreMaxPeers ? peerCount : kCoreMaxPeers;
        for (int i = 0; i < n; ++i) {
            const Peer& p = s.peers[i];
            if (p.at == 0 || (peerOk && !peerOk[i])) {
                ForgetPeer(s, i);
                continue;
            }
            const bool fresh = s.seenAt[i] != p.at;
            if (!fresh && now < s.pupNext[i])
                continue;
            s.seenAt[i] = p.at;
            s.pupNext[i] = now + kEveryMs;
            if (now < p.at || now - p.at >= kElapsedCapMs)
                continue;

            const int w = PickWave(s, now, waves, waveCount, p.female);
            if (!p.handle || w < 0)
                continue;                 // ext logs ok=0 but starts no voice
            PeerCue cue;
            cue.slot = i;
            cue.wave = w;
            cue.handle = p.handle;
            for (int j = 0; j < 3; ++j)
                cue.pos[j] = p.pos[j];
            cue.hasPos = p.pos[0] != 0.0f || p.pos[1] != 0.0f || p.pos[2] != 0.0f;
            cue.fresh = fresh;
            out.peers[out.peerCount++] = cue;
        }
        return out;
    }

    inline void Reset(State& s) noexcept { s = State{}; }

} // namespace net::audio::stun_cough::core
