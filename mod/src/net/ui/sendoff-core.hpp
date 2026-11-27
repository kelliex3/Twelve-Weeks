// Pure rules of the menu send-off (tools/ext/sendoff.c + feature_match_horn's
// shared envelope). No engine / Windows dependency; the native owner owns all
// DC spawns, widget fades and PCM.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace menu::sendoff::core {

    inline constexpr int kWlf = 1, kSer = 2;
    inline constexpr uint64_t kCrowsMs = 2200, kFacMs = 3800;
    inline constexpr uint64_t kFullMs = 5500, kFadeMs = 7600;
    inline constexpr uint64_t kFleeJitMs = 600;
    inline constexpr uint64_t kUiMs = 500, kHornMs = 600;
    inline constexpr uint64_t kBlackMs = 5200, kEndMs = 7700, kMaxMs = 11000;
    inline constexpr float kBlackFadeS = 1.0f, kDipDb = -10.0f;
    inline constexpr uint64_t kDipMs = 600;
    inline constexpr float kCompCap = 4.0f;
    inline constexpr float kRad = 3.14159265f / 180.0f;

    inline constexpr int kNb = 16;
    inline constexpr float kFarWlf = 24.0f, kFarSer = 14.0f, kSpread = 6.0f;

    enum Route : int { kNone = 0, kFlee = 1, kExt = 2, kFx = 4 };
    inline constexpr std::array<int, 3> kRoute{{kNone, kExt, kExt}};

    // ext MS_POSE / MS_FOV for the two faction menu scenes: camera xyz, target xyz.
    inline constexpr std::array<float, 6> kWlfPose{{-26.72f, -5.21f, 1036.27f,
                                                     -44.42f, -1.07f, 1027.95f}};
    inline constexpr std::array<float, 6> kSerPose{{272.87f, 18.17f, 52.01f,
                                                     253.20f, 19.26f, 48.56f}};
    inline constexpr float kWlfFov = 30.6f, kSerFov = 51.6f, kDefaultFov = 75.0f;

    struct CamBasis {
        float c[3]{}, f[3]{}, r[3]{}, u[3]{}, th = 0.0f;
    };

    // so_cam: right = screen right, up flipped so it points up on screen.
    [[nodiscard]] inline CamBasis Camera(int scene) noexcept
    {
        const auto& d = scene == kWlf ? kWlfPose : kSerPose;
        CamBasis b;
        for (int k = 0; k < 3; ++k) b.c[k] = d[k];
        const float fx = d[3] - d[0], fy = d[4] - d[1], fz = d[5] - d[2];
        const float l = std::sqrt(fx * fx + fy * fy + fz * fz);
        b.f[0] = l > 1e-4f ? fx / l : 0.0f;
        b.f[1] = l > 1e-4f ? fy / l : 0.0f;
        b.f[2] = l > 1e-4f ? fz / l : 1.0f;
        const float rl = std::sqrt(b.f[2] * b.f[2] + b.f[0] * b.f[0]);
        if (rl > 1e-4f) {
            b.r[0] = -b.f[2] / rl;
            b.r[2] = b.f[0] / rl;
        }
        b.u[0] = b.r[1] * b.f[2] - b.r[2] * b.f[1];
        b.u[1] = b.r[2] * b.f[0] - b.r[0] * b.f[2];
        b.u[2] = b.r[0] * b.f[1] - b.r[1] * b.f[0];
        if (b.u[1] < 0.0f) {
            for (int k = 0; k < 3; ++k) b.u[k] = -b.u[k];
            b.r[0] = -b.r[0];
            b.r[2] = -b.r[2];
        }
        const float fov = scene == kWlf ? kWlfFov : kSerFov;
        b.th = std::tan((fov > 1.0f ? fov : kDefaultFov) * 0.5f * kRad);
        return b;
    }

    // Quaternion with the view axis (+Z) along `v` (ext so_quat_dir / mcam_quat).
    inline void QuatDir(const float v[3], float q[4]) noexcept
    {
        if (!v || !q) return;
        const float h = std::sqrt(v[0] * v[0] + v[2] * v[2]);
        const float yaw = std::atan2(v[0], v[2]);
        const float pit = -std::atan2(v[1], h > 1e-4f ? h : 1e-4f);
        const float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f);
        const float cp = std::cos(pit * 0.5f), sp = std::sin(pit * 0.5f);
        q[0] = cy * sp;
        q[1] = sy * cp;
        q[2] = -sy * sp;
        q[3] = cy * cp;
    }

    // xorshift32: the ext's so_rng / so_rf, kept deterministic.
    [[nodiscard]] inline uint32_t NextRand(uint32_t& s) noexcept
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }

    [[nodiscard]] inline float Rand01(uint32_t& s) noexcept
    {
        return static_cast<float>(NextRand(s) & 0xffffffu) / 16777216.0f;
    }

    // Horn envelopes (feature_match_horn) shared with the sound timeline.
    [[nodiscard]] inline float Duck(uint64_t t) noexcept
    {
        const float x = t >= kDipMs ? 1.0f : static_cast<float>(t) / static_cast<float>(kDipMs);
        const float low = std::pow(10.0f, kDipDb / 20.0f);
        return 1.0f + (low - 1.0f) * (0.5f - 0.5f * std::cos(3.14159265f * x));
    }

    [[nodiscard]] inline float BlackFade(uint64_t t) noexcept
    {
        if (t <= kBlackMs) return 1.0f;
        const uint64_t span = kEndMs - kBlackMs - 100;
        float v = span ? 1.0f - static_cast<float>(t - kBlackMs) / static_cast<float>(span) : 0.0f;
        return v < 0.0f ? 0.0f : v;
    }

    // Voice gain so a voice sounds at `level` after the current session volume.
    [[nodiscard]] inline float Compensate(float level, float session) noexcept
    {
        const float s = session > 1e-3f ? session : 1e-3f;
        const float c = level / s;
        return c > kCompCap ? kCompCap : c;
    }

    // so_env: the faction clip's own fade into the black and down with the music.
    [[nodiscard]] inline float Environment(uint64_t t) noexcept
    {
        if (t < kFullMs) return 1.0f;
        if (t >= kFadeMs) return 0.0f;
        const float x = static_cast<float>(kFadeMs - t)
                      / static_cast<float>(kFadeMs - kFullMs);
        return x * x;
    }

    struct Bird {
        float p[3]{}, v[3]{}, acc[3]{};
        int delay = 0;                  // staggered takeoff, ms
        int phase = 0;                  // 0 takeoff, 1 fly cycle
        uint64_t animAt = 0;
        int animMs = 0;
        uint64_t obj = 0;
    };

    struct Flock {
        int route = kNone;
        int scene = 0;
        int n = 0;
        int dir = 1;                    // +1 from the left, -1 from the right
        float far = 0.0f, spread = 0.0f;
        uint64_t goMs = 0;
        std::array<Bird, kNb> b{};
    };

    [[nodiscard]] inline float Far(int scene) noexcept
    {
        return scene == kWlf ? kFarWlf : kFarSer;
    }

    // Set up one crow: start just off one side of the frame, below the bottom
    // edge, climbing away. Speeds scale with distance (SO_FAR_*).
    inline void PlaceBird(Bird& b, const CamBasis& cam, int i, int dir,
                          float d, float th, uint32_t& rng) noexcept
    {
        const float ks = std::sqrt(d / 5.5f);
        const float hw = d * th * (16.0f / 9.0f);
        const float lat = -static_cast<float>(dir) * (0.45f + 0.35f * Rand01(rng)) * hw;
        const float hgt = -(1.04f + 0.10f * Rand01(rng)) * d * th;
        const float up = (3.2f + Rand01(rng)) * ks;
        const float fw = (3.5f + 2.0f * Rand01(rng)) * ks;
        const float sd = static_cast<float>(dir) * (1.2f + 1.2f * Rand01(rng)) * ks;
        for (int k = 0; k < 3; ++k) {
            b.p[k] = cam.c[k] + cam.f[k] * d + cam.r[k] * lat + cam.u[k] * hgt;
            b.v[k] = cam.f[k] * fw + cam.u[k] * up + cam.r[k] * sd;
            b.acc[k] = (cam.f[k] * (2.5f + Rand01(rng))
                        - cam.u[k] * (1.2f + 0.4f * Rand01(rng)) + cam.r[k] * sd * 0.3f) * ks;
        }
        (void)i;                 // the ext spreads birds through the RNG, not the index
        b.delay = static_cast<int>(Rand01(rng) * 450.0f);
        b.phase = 0;
        b.animAt = 0;
        b.animMs = 0;
    }

    // One frame of a bird's flight. Returns false when the engine object is
    // gone (the owner clears the handle); the position/quaternion are for the
    // owner's set-object-locator call.
    [[nodiscard]] inline bool StepBird(Bird& b, const CamBasis& cam, float dt,
                                       float elapsedSec, int i, float p[3],
                                       float q[4]) noexcept
    {
        const float bt = elapsedSec - static_cast<float>(b.delay) / 1000.0f;
        if (bt < 0.0f) return true;
        for (int k = 0; k < 3; ++k) {
            const float wob = (k == 1 ? 0.6f : 0.35f)
                            * std::sin(bt * (5.0f + static_cast<float>(i % 5))
                                       + static_cast<float>(i) * 1.7f);
            b.v[k] += (b.acc[k] + wob) * dt;
        }
        float vu = b.v[0] * cam.u[0] + b.v[1] * cam.u[1] + b.v[2] * cam.u[2];
        if (vu < 0.15f)
            for (int k = 0; k < 3; ++k) b.v[k] += (0.15f - vu) * cam.u[k];
        for (int k = 0; k < 3; ++k) {
            b.p[k] += b.v[k] * dt;
            p[k] = b.p[k];
        }
        QuatDir(b.v, q);
        return true;
    }

    [[nodiscard]] inline bool FlyAnimDue(const Bird& b, uint64_t now,
                                         float elapsedSec, int animMs) noexcept
    {
        const uint64_t bt = elapsedSec >= static_cast<float>(b.delay) / 1000.0f
            ? static_cast<uint64_t>((elapsedSec - static_cast<float>(b.delay) / 1000.0f) * 1000.0f)
            : 0;
        if (b.phase == 0 && bt > 700)
            return true;
        if (b.phase == 1 && b.animMs > 0 && now >= b.animAt
            && now - b.animAt >= static_cast<uint64_t>(b.animMs))
            return true;
        (void)animMs;
        return false;
    }

} // namespace menu::sendoff::core
