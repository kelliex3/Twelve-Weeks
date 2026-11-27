// Pure rules of the title-screen background (tools/ext/titlebg.c): Hillcrest
// overlook camera pose, slow sway, boot-cover readiness and the render-layer
// sidecar. The native owner owns the DC camera / render / fade calls.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace menu::titlebg::core {

    // x y z, yaw deg (0 = +Z, atan2(fx, fz)), pitch deg DOWN, fov deg.
    inline constexpr std::array<float, 6> kPose{{14.855f, 2.957f, 530.968f,
                                                  150.19f, -3.23f, 46.94f}};
    inline constexpr float kYawDeg = 0.15f, kYawS = 47.0f;
    inline constexpr float kPitchDeg = 0.06f, kPitchS = 31.0f;
    inline constexpr float kPosM = 0.04f;
    inline constexpr float kRampS = 4.0f;
    inline constexpr float kTau = 6.28318530f;

    inline constexpr int kBootHits = 6, kBootRs = 3;
    inline constexpr uint64_t kBootSettleMs = 2500, kBootMaxMs = 9000;
    inline constexpr float kBootFadeS = 1.2f;
    inline constexpr int kMaxLayers = 4;
    inline constexpr int kRsPrio = 100;
    inline constexpr const char* kDefaultLayer = "*res-rs-005-towerhill-vista*";

    struct Pose {
        float p[3]{}, yaw = 0.0f, pitch = 0.0f, fov = 0.0f;
    };

    // t2r-title-cam.txt: five values required, the sixth (fov) optional.
    [[nodiscard]] inline bool ParsePose(const char* line, float out[6]) noexcept
    {
        if (!line || !out) return false;
        float v[6]{};
        const int n = std::sscanf(line, "%f %f %f %f %f %f",
                                  &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
        if (n < 5) return false;
        for (int i = 0; i < 6; ++i) out[i] = v[i];
        return true;
    }

    [[nodiscard]] inline float Fov(const float b[6]) noexcept
    {
        return b && b[5] > 0.0f ? b[5] : kPose[5];
    }

    [[nodiscard]] inline float Ease(float t) noexcept
    {
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        return t * t * (3.0f - 2.0f * t);
    }

    // The first frame is exactly the file/default pose; the sway eases in over
    // four seconds so the boot black never reveals a jump.
    [[nodiscard]] inline Pose FramePose(uint64_t now, uint64_t t0,
                                        const float b[6]) noexcept
    {
        const float src[6] = { b ? b[0] : kPose[0], b ? b[1] : kPose[1],
                              b ? b[2] : kPose[2], b ? b[3] : kPose[3],
                              b ? b[4] : kPose[4], b ? b[5] : kPose[5] };
        const float t = static_cast<float>(now >= t0 ? now - t0 : 0) / 1000.0f;
        const float k = Ease(t / kRampS);
        Pose out;
        out.p[0] = src[0] + k * kPosM * std::sin(kTau * t / 61.0f);
        out.p[1] = src[1] + k * kPosM * 0.5f * std::sin(kTau * t / 37.0f + 0.7f);
        out.p[2] = src[2] + k * kPosM * std::sin(kTau * t / 53.0f + 2.1f);
        out.yaw = src[3] + k * kYawDeg * std::sin(kTau * t / kYawS);
        out.pitch = src[4] + k * kPitchDeg * std::sin(kTau * t / kPitchS + 1.3f);
        out.fov = Fov(src);
        return out;
    }

    // yaw(Y) * pitch(X), +Z forward (the ext's mcam_quat).
    inline void Quat(float yawDeg, float pitchDeg, float q[4]) noexcept
    {
        if (!q) return;
        const float cy = std::cos(yawDeg * 0.5f * 3.14159265f / 180.0f);
        const float sy = std::sin(yawDeg * 0.5f * 3.14159265f / 180.0f);
        const float cp = std::cos(pitchDeg * 0.5f * 3.14159265f / 180.0f);
        const float sp = std::sin(pitchDeg * 0.5f * 3.14159265f / 180.0f);
        q[0] = cy * sp;
        q[1] = sy * cp;
        q[2] = -sy * sp;
        q[3] = cy * cp;
    }

    struct Layer {
        char name[96]{};
        int prio = kRsPrio;
        float str = 1.0f;
    };

    [[nodiscard]] inline bool ParseLayer(const char* line, Layer& out) noexcept
    {
        if (!line || line[0] == '#') return false;
        char name[96];
        int prio = 0;
        float str = 0.0f;
        if (std::sscanf(line, "%95s %d %f", name, &prio, &str) != 3) return false;
        std::snprintf(out.name, sizeof out.name, "%s", name);
        out.prio = prio;
        out.str = str;
        return true;
    }

    inline void DefaultLayer(Layer& out) noexcept
    {
        std::snprintf(out.name, sizeof out.name, "%s", kDefaultLayer);
        out.prio = kRsPrio;
        out.str = 1.0f;
    }

    // Boot black cover: 0 not started, 1 holding, 2 released. Ready needs the
    // title page, camera hook hits and a live render-settings handle, then a
    // fixed settle; the timeout is a safety valve.
    struct Boot {
        int state = 0;
        uint64_t t0 = 0, titleAt = 0, readyAt = 0, fadeAt = 0;
        uint32_t hits0 = 0;
        int rsOk = 0;
    };

    [[nodiscard]] inline bool BootCanStart(uint64_t frame, int matchSubstate) noexcept
    {
        return frame > 5 && matchSubstate == 0;
    }

    inline void BootBegin(Boot& b, uint64_t now, uint32_t hits) noexcept
    {
        b = Boot{};
        b.state = 1;
        b.t0 = now;
        b.hits0 = hits;
    }

    // Returns true once on the frame the fade-up starts.
    [[nodiscard]] inline bool BootTick(Boot& b, uint64_t now, uint32_t hits,
                                       bool rsHandle, bool titleUp) noexcept
    {
        if (b.state != 1) return false;
        if (rsHandle && b.rsOk < 1000) ++b.rsOk;
        if (titleUp && !b.titleAt) b.titleAt = now;
        const bool ready = titleUp && hits - b.hits0 >= static_cast<uint32_t>(kBootHits)
                        && b.rsOk >= kBootRs;
        if (ready && !b.readyAt) b.readyAt = now;
        if (!ready) b.readyAt = 0;
        const bool timeout = (b.titleAt && now - b.titleAt >= kBootMaxMs)
                          || now - b.t0 >= 60000;
        if (!(b.readyAt && now - b.readyAt >= kBootSettleMs) && !timeout) return false;
        b.state = 2;
        b.fadeAt = now;
        return true;
    }

    [[nodiscard]] inline float ButtonAlpha(uint64_t now, const Boot& b) noexcept
    {
        if (!b.fadeAt) return 1.0f;
        const float u = static_cast<float>(now >= b.fadeAt ? now - b.fadeAt : 0)
                      / (kBootFadeS * 1000.0f);
        return u >= 1.0f ? 1.0f : Ease(u);
    }

} // namespace menu::titlebg::core
