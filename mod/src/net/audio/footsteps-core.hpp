#pragma once

// Pure remote-footstep rules (tools/ext/footsteps.c): sidecar configuration,
// the authored tier range curve and near-field share, stance / gait selection,
// ankle plants with a speed-cadence fallback, and step-to-step wave / gain
// spread. No engine, PCM, network, sidecar-file or private-data read happens
// here: the native owner in footsteps.cpp supplies the peers, joints, sidecar
// text, data view and PCM sink. Tests: mod/tests/footsteps_core_test.cpp.
//
// The generated FS_WAVE / FS_TIER table and footsteps.pcm are private and do
// not ship here. With no data view the owner does not start any voice.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace net::audio::footsteps::core {

    inline constexpr int kTierCount = 4;      // sprint, run, walk, crouch
    inline constexpr int kMaxStepsPerTick = 2;

    enum class TierId : uint8_t { kSprint = 0, kRun, kWalk, kCrouch };

    inline constexpr float kProneHead = 0.60f;
    inline constexpr float kLift = 0.055f;
    inline constexpr float kPlant = 0.025f;
    inline constexpr float kRefractMs = 170.0;
    inline constexpr int   kAnkleFail = 45;
    inline constexpr double kAnkleRetryMs = 5000.0;
    inline constexpr float kAgility2Gain = 0.0f; // 2013: silent at every gait

    struct TierCfg { float gain = 0.0f, d0 = 0.0f, dmax = 0.0f; };
    struct NearCfg { float keep = 0.5f, lo = 1.5f, hi = 5.0f; };

    struct Config {
        TierCfg tier[kTierCount] = {
            {0.34f, 3.0f, 35.0f}, {0.26f, 3.0f, 28.0f},
            {0.13f, 2.0f, 16.0f}, {0.045f, 1.0f, 7.0f}
        };
        NearCfg near;
        float   walk = 0.5f, run = 2.6f, sprint = 4.6f;
        float   crouchHead = 1.25f;
        bool    forceCadence = false;
    };

    inline constexpr const char* const kTierName[kTierCount] = {
        "sprint", "run", "walk", "crouch"
    };
    inline constexpr float kCadenceMs[kTierCount] = { 300.0f, 360.0f, 550.0f, 600.0f };

    // Parse the whole t2r-footsteps.txt body. Each accepted line updates only
    // its own field; malformed / out-of-range lines are ignored, like ext.
    inline bool Parse(const char* text, Config& cfg) noexcept
    {
        if (!text)
            return false;
        bool used = false;
        char line[256];
        const char* p = text;
        while (*p) {
            size_t n = 0;
            while (*p && *p != '\n' && n + 1 < sizeof line)
                line[n++] = *p++;
            line[n] = 0;
            if (*p == '\n')
                ++p;
            char key[32];
            float a = 0.0f, b = 0.0f, c = 0.0f;
            if (std::sscanf(line, "%31s %f %f %f", key, &a, &b, &c) < 2)
                continue;
            int t = -1;
            for (int i = 0; i < kTierCount; ++i)
                if (!std::strcmp(key, kTierName[i]))
                    t = i;
            if (t >= 0) {
                if (a >= 0.0f && a <= 2.0f && b >= 0.0f && c > b) {
                    cfg.tier[t] = { a, b, c };
                    used = true;
                }
            }
            else if (!std::strcmp(key, "near")) {
                if (a >= 0.0f && a <= 1.0f && c > b) {
                    cfg.near = { a, b, c };
                    used = true;
                }
            }
            else if (!std::strcmp(key, "speed")) {
                if (a > 0.0f && b > a && c > b) {
                    cfg.walk = a;
                    cfg.run = b;
                    cfg.sprint = c;
                    used = true;
                }
            }
            else if (!std::strcmp(key, "crouch-head")) {
                if (a > 0.3f && a < 2.0f) {
                    cfg.crouchHead = a;
                    used = true;
                }
            }
            else if (!std::strcmp(key, "cadence")) {
                cfg.forceCadence = (a != 0.0f);
                used = true;
            }
        }
        return used;
    }

    inline float DistGain(int tier, float d, const Config& cfg) noexcept
    {
        if (tier < 0 || tier >= kTierCount)
            return 0.0f;
        const TierCfg& t = cfg.tier[tier];
        if (d <= t.d0)
            return 1.0f;
        if (d >= t.dmax || t.dmax <= t.d0)
            return 0.0f;
        const float f = (t.dmax - d) / (t.dmax - t.d0);
        return f * f;
    }

    inline float Near(float d, const Config& cfg) noexcept
    {
        const NearCfg& n = cfg.near;
        if (d >= n.hi)
            return 1.0f;
        if (d <= n.lo || n.hi <= n.lo)
            return n.keep;
        float t = (d - n.lo) / (n.hi - n.lo);
        t = t * t * (3.0f - 2.0f * t);
        return n.keep + (1.0f - n.keep) * t;
    }

    inline int TierFor(float v, float vy, bool headOk, float headH,
                       const Config& cfg, float speedScale) noexcept
    {
        (void)vy;                              // vertical speed gates `moving`, not the tier
        if (headOk && headH < kProneHead)
            return -1;                                     // prone: no steps
        if (headOk && headH < cfg.crouchHead)
            return static_cast<int>(TierId::kCrouch);
        if (v > cfg.sprint * speedScale)
            return static_cast<int>(TierId::kSprint);
        if (v > cfg.run * speedScale)
            return static_cast<int>(TierId::kRun);
        return static_cast<int>(TierId::kWalk);
    }

    // ---- private data view ----------------------------------------------------

    struct Wave { uint32_t off = 0, n = 0, rate = 0; };
    struct Tier { uint16_t first = 0, count = 0; };
    struct DataView {
        const Wave*  waves = nullptr;
        int          waveCount = 0;
        const Tier*  tiers = nullptr;
        int          tierCount = 0;
        uint32_t     rate = 0;
        uint32_t     samples = 0;
    };

    struct Step {
        int      tier = 0;
        int      wave = 0;
        float    gain = 0.0f;
        float    distance = 0.0f;
        float    rnear = 0.0f;
        float    rfar = 0.0f;
        uint32_t rate = 0;
        int      foot = 0;
        bool     ankle = false;
    };

    struct Result {
        Step steps[kMaxStepsPerTick];
        int  count = 0;
    };

    // One peer's gait state (ext struct fs_slot). The owner keeps one per slot
    // and clears the whole array when the PCM layer is off or the match ends.
    struct Peer {
        uint64_t h = 0;
        bool     have = false;
        float    last[3] = {0, 0, 0};
        double   lastMs = 0.0;
        float    v = 0.0f, vy = 0.0f;
        float    headH = 0.0f;
        bool     headOk = false;
        float    base[2] = {1e9f, 1e9f};
        bool     armed[2] = {false, false};
        double   stepMs[2] = {0.0, 0.0};
        double   anyMs = 0.0;
        bool     ankles = true;
        int      ankleMiss = 0;
        double   ankleRetryMs = 0.0;
        float    phase = 0.0f;
        int      tier = 0;
        int      lastWave = -1;
        unsigned steps = 0;
        unsigned played = 0;
    };

    inline void ResetPeer(Peer& p) noexcept { p = Peer{}; }

    struct Input {
        uint64_t body = 0;          // puppet SID; a change reseeds the gait state
        double   nowMs = 0.0;
        uint64_t tick = 0;          // GetTickCount64, for the wave mixer
        uint32_t slot = 0;
        float    pos[3] = {0, 0, 0};
        bool     havePos = false;
        bool     sampleHead = false; // every 4th frame, per slot
        float    headPos[3] = {0, 0, 0};
        bool     haveAnkles = false;
        float    anklePos[2][3] = {};
        int      agilityLevel = 0;
        float    speedScale = 1.0f; // native slowdown feed: owner passes Mul()
        float    distance = 0.0f;   // listener distance from the puppet
    };

    inline void AddStep(Result& r, const Step& s) noexcept
    {
        if (r.count < kMaxStepsPerTick)
            r.steps[r.count++] = s;
    }

    // Emit one authored step. `distance` is the listener distance supplied by
    // the owner; the returned rnear/rfar neutralize the engine's own 3-30 m
    // curve so the tier curve here is the only distance model.
    inline void Emit(Peer& p, const DataView& d, const Config& cfg,
                     const Input& in, int tier, int foot, bool ankle,
                     Result& out) noexcept
    {
        ++p.steps;
        if (!d.tiers || tier < 0 || tier >= d.tierCount)
            return;
        const Tier& t = d.tiers[tier];
        if (!t.count)
            return;
        const uint64_t r = (in.tick * 2654435761ull)
            ^ (static_cast<uint64_t>(in.slot) * 977u + static_cast<uint64_t>(p.steps) * 131u);
        int k = static_cast<int>(r % t.count);
        if (t.count > 1 && static_cast<int>(t.first) + k == p.lastWave)
            k = (k + 1 + static_cast<int>((r >> 8) % (t.count - 1))) % t.count;
        const int w = static_cast<int>(t.first) + k;
        p.lastWave = w;

        float g = cfg.tier[tier].gain
                * DistGain(tier, in.distance, cfg)
                * Near(in.distance, cfg);
        if (in.agilityLevel >= 2)
            g *= kAgility2Gain;
        if (g < 0.003f)
            return;
        g *= 0.9f + 0.2f * static_cast<float>((r >> 16) & 0xff) / 255.0f;

        Step s;
        s.tier = tier;
        s.wave = w;
        s.gain = g;
        s.distance = in.distance;
        s.rnear = cfg.tier[tier].dmax;
        s.rfar = cfg.tier[tier].dmax + 0.1f;
        s.rate = in.havePos && d.waves && w >= 0 && w < d.waveCount
            ? d.waves[w].rate : d.rate;
        s.foot = foot;
        s.ankle = ankle;
        AddStep(out, s);
    }

    inline int Tick(Peer& p, const DataView& d, const Config& cfg,
                    const Input& in, Result& out) noexcept
    {
        if (!in.havePos)
            return 0;
        if (!p.have || (in.body && p.h != in.body)) {
            ResetPeer(p);                      // new puppet: spawn / respawn / reconnect
            p.h = in.body;
            for (int i = 0; i < 3; ++i)
                p.last[i] = in.pos[i];
            p.lastMs = in.nowMs;
            p.have = true;
            return 0;
        }

        float dt = static_cast<float>((in.nowMs - p.lastMs) * 0.001);
        if (dt < 0.004f)
            return 0;
        if (dt > 0.25f)
            dt = 0.25f;
        const float dx = in.pos[0] - p.last[0];
        const float dz = in.pos[2] - p.last[2];
        const float dy = in.pos[1] - p.last[1];
        const float vi = std::sqrt(dx * dx + dz * dz) / dt;
        const float vyi = dy / dt;
        for (int i = 0; i < 3; ++i)
            p.last[i] = in.pos[i];
        p.lastMs = in.nowMs;
        if (vi > 15.0f) {                       // teleport / respawn snap
            p.v = 0.0f;
            p.armed[0] = p.armed[1] = false;
            return 0;
        }
        float a = dt / 0.15f;
        if (a > 1.0f)
            a = 1.0f;
        p.v += (vi - p.v) * a;
        p.vy += (vyi - p.vy) * a;
        if (in.sampleHead) {
            p.headOk = true;
            p.headH = in.headPos[1] - in.pos[1];
        }
        const bool prone = p.headOk && p.headH < kProneHead;
        const bool crouch = p.headOk && !prone && p.headH < cfg.crouchHead;
        const int tier = TierFor(p.v, p.vy, p.headOk, p.headH, cfg, in.speedScale);
        p.tier = tier < 0 ? 0 : tier;
        const bool moving = tier >= 0 && p.v > cfg.walk && std::fabs(p.vy) < 2.0f;

        bool useAnkles = p.ankles && !cfg.forceCadence;
        if (!p.ankles && !cfg.forceCadence && in.nowMs - p.ankleRetryMs > kAnkleRetryMs) {
            p.ankles = true;
            p.ankleMiss = 0;
            useAnkles = true;
        }
        if (useAnkles) {
            if (!in.haveAnkles) {
                if (++p.ankleMiss >= kAnkleFail) {
                    p.ankles = false;
                    p.ankleRetryMs = in.nowMs;
                }
            }
            else {
                p.ankleMiss = 0;
                const float hf[2] = {
                    in.anklePos[0][1] - in.pos[1],
                    in.anklePos[1][1] - in.pos[1]
                };
                for (int f = 0; f < 2; ++f) {
                    if (!std::isfinite(hf[f]) || hf[f] < -1.0f || hf[f] > 1.5f)
                        continue;
                    if (p.base[f] > 1e8f)
                        p.base[f] = hf[f];
                    if (hf[f] < p.base[f])
                        p.base[f] = hf[f];
                    else {
                        float k = dt / 1.5f;
                        if (k > 1.0f)
                            k = 1.0f;
                        p.base[f] += (hf[f] - p.base[f]) * k;
                    }
                    const float lift = crouch ? kLift * 0.7f : kLift;
                    if (hf[f] > p.base[f] + lift)
                        p.armed[f] = true;
                    else if (p.armed[f] && hf[f] < p.base[f] + kPlant
                             && in.nowMs - p.stepMs[f] > kRefractMs) {
                        p.armed[f] = false;
                        p.stepMs[f] = in.nowMs;
                        if (moving) {
                            p.anyMs = in.nowMs;
                            Emit(p, d, cfg, in, tier, f, true, out);
                        }
                    }
                }
            }
        }
        else if (moving) {
            p.phase += static_cast<float>(dt * 1000.0 / kCadenceMs[p.tier]);
            if (p.phase >= 1.0f) {
                p.phase -= 1.0f;
                if (p.phase > 1.0f)
                    p.phase = 0.0f;
                p.anyMs = in.nowMs;
                Emit(p, d, cfg, in, tier, static_cast<int>(p.steps & 1u), false, out);
            }
        }
        else
            p.phase = 0.5f;
        return out.count;
    }

} // namespace net::audio::footsteps::core
