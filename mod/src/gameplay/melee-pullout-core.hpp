#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Pure rules for the port of tools/ext/melee_pullout.c. No engine / net headers:
// the bridge supplies the clock, input edge, peer facts and wire transport.
//
// The native path keeps the ext's exact MSG_MEX envelope ([5][op][id][0][payload])
// and the 400 ms counter window 600 ms before the 3.2 s drag hand-off. The SP
// camera below is the ext's table-free procedural pull-out composition (its
// inline PCP_* tables); the private authored pullout_cam_tracks.h replay is not
// in this checkout, so no native path can claim that exact shot.
namespace gameplay::melee_pullout::core {

    inline constexpr uint8_t  kFamily   = 5;
    inline constexpr uint8_t  kIntro    = 11;
    inline constexpr uint8_t  kResult   = 12;
    inline constexpr uint8_t  kEnd      = 13;
    inline constexpr uint8_t  kCamera   = 14;

    inline constexpr uint16_t kWinMs    = 400;
    inline constexpr uint16_t kLeadMs   = 600;
    inline constexpr uint16_t kGraceMs  = 30;
    inline constexpr uint16_t kResultWaitMs = 220;
    inline constexpr uint16_t kLateMs   = 1200;
    inline constexpr uint16_t kCameraMs = 150;

    inline constexpr uint32_t kDragMs   = 3200; // pu-drag-{abby,ellie} end_s
    inline constexpr uint32_t kBlowMs   = 1000; // pu-{ok,fail} dmax
    inline constexpr uint32_t kOutMs    = 1750; // pu-fail end_s
    inline constexpr uint32_t kCoolFailMs = 3000;
    inline constexpr uint32_t kCoolOkMs   = 8000;

    inline constexpr float kRange       = 2.5f;
    inline constexpr float kVertical    = 1.2f;
    inline constexpr float kConeDeg     = 55.0f;
    inline constexpr float kProneHead   = 0.60f;
    inline constexpr float kCounterDamage = 34.0f; // 2 x FIST_DAMAGE (17)

    // INTRO payload byte: state, attacker-side direction, grapple / pounce flags and
    // the variant. The native path sends the plain standing drag (dir 0, no flags).
    inline uint8_t IntroBits(bool set, uint8_t dir, bool grapple,
                             bool pounce, uint8_t variant) noexcept
    {
        const uint8_t d = dir > 3 ? 3 : dir;
        const uint8_t v = variant > 3 ? 3 : variant;
        return static_cast<uint8_t>((set ? 1u : 0u) | (d << 1) |
                                    (grapple ? 8u : 0u) | (pounce ? 16u : 0u) |
                                    (v << 5));
    }

    struct Bits {
        bool    set = false;
        uint8_t dir = 0;
        bool    grapple = false;
        bool    pounce = false;
        uint8_t variant = 0;
    };

    inline Bits DecodeIntro(uint8_t b) noexcept
    {
        return { (b & 1u) != 0, static_cast<uint8_t>((b >> 1) & 3u),
                 (b & 8u) != 0, (b & 16u) != 0,
                 static_cast<uint8_t>((b >> 5) & 3u) };
    }

    struct Window {
        uint64_t open = 0;
        uint64_t close = 0;
    };

    // ext: open = shownEnd - 600; close = max(open, shownAt) + 400 + 30,
    // capped at shownEnd + 220 - 80 so a late-shown window still lands before the
    // attacker's verdict wait. `shownAt` 0 means the prompt appeared at `open`.
    inline Window CounterWindow(uint64_t end, uint64_t shownAt = 0) noexcept
    {
        if (!end)
            return {};
        const uint64_t open = end > kLeadMs ? end - kLeadMs : 1ull;
        const uint64_t base = shownAt && shownAt > open ? shownAt : open;
        uint64_t close = base + static_cast<uint64_t>(kWinMs) + kGraceMs;
        const uint64_t cap = end + static_cast<uint64_t>(kResultWaitMs) - 80u;
        if (close > cap)
            close = cap;
        if (close < open)
            close = open;
        return { open, close };
    }

    enum class PressResult : uint8_t { kNone, kBurn, kCounter, kFail };

    struct PressState {
        bool burnt = false;
        bool decided = false;
        bool countered = false;
        uint16_t relMs = 0;
    };

    // One shared-input step for the victim. A press before `open` burns this
    // counter; a press inside the window sends a valid verdict; after `close`
    // (or no press at all) the window books a fail. `timedOut` lets the caller
    // close the window on the clock even when no edge was seen.
    inline PressResult Step(uint64_t now, const Window& w, bool press,
                            bool timedOut, PressState& st) noexcept
    {
        if (!w.open || !w.close || st.decided)
            return PressResult::kNone;
        if (press && now < w.open) {
            if (!st.burnt) {
                st.burnt = true;
                return PressResult::kBurn;
            }
            return PressResult::kNone;
        }
        // A burnt counter stays burnt: the ext never rescues it with a second press
        // inside the window, and the window still books the fail on its close.
        if (press && st.burnt && now <= w.close)
            return PressResult::kNone;
        if (press && !st.burnt && now <= w.close) {
            st.decided = true;
            st.countered = true;
            st.relMs = static_cast<uint16_t>(now - w.open);
            return PressResult::kCounter;
        }
        if ((timedOut || (press && now > w.close)) && !st.countered) {
            st.decided = true;
            st.countered = false;
            st.relMs = static_cast<uint16_t>(now > w.open ? now - w.open : 0);
            return PressResult::kFail;
        }
        return PressResult::kNone;
    }

    // Behind the prone victim: dot(backward, attacker-relative) >= cos(half angle).
    // A covered victim may be taken from any side; that flag stays with the bridge.
    inline bool BehindCone(float meX, float meZ, float victimX, float victimZ,
                           float backX, float backZ, bool covered) noexcept
    {
        if (!std::isfinite(meX + meZ + victimX + victimZ + backX + backZ))
            return false;
        if (covered)
            return true;
        const float dx = meX - victimX, dz = meZ - victimZ;
        const float d = std::sqrt(dx * dx + dz * dz);
        const float b = std::sqrt(backX * backX + backZ * backZ);
        if (!(d > 1e-4f) || !(b > 1e-4f))
            return false;
        const float cosHalf = std::cos(kConeDeg * 3.14159265f / 180.0f);
        return (dx * backX + dz * backZ) / (d * b) >= cosHalf;
    }

    // ext's fail blow is the victim's replicated hp + 1 (100 -> 101, a down),
    // falling back to 100 when no sane hp has been reported.
    inline float FailDamage(float netHp) noexcept
    {
        return netHp > 0.0f && netHp < 100000.0f ? netHp + 1.0f : 100.0f;
    }

    inline bool CooldownReady(uint64_t now, uint64_t until) noexcept
    {
        return !until || now >= until;
    }

    // ---- SP camera -------------------------------------------------------------------
    // tools/ext/melee_pullout.c's procedural pull-out camera (g_xcam_pulproc),
    // without the private authored replay tables. These are the ext's own
    // inline keyframes; the bridge blends them from the live camera and stops
    // writing after the hand-back.
    inline constexpr float kSpSwingSecs = 1.6f;   // PUL_V2_SWING_S
    inline constexpr float kSpHandBackMs = 1500.0f;
    inline constexpr float kSpCamIndex = 1.0f;    // CAM byte: 1 = this native track

    inline constexpr float kPcpDt = 0.2667f;
    inline constexpr float kPcpD[13] = {
        1.35f, 1.29f, 1.27f, 1.28f, 1.34f, 1.36f, 1.17f,
        1.24f, 1.21f, 1.28f, 1.27f, 1.19f, 1.17f
    };
    inline constexpr float kPcpOff[13] = {
        22.0f, 21.0f, 23.0f, 22.0f, 19.0f, 16.0f, 12.0f,
        14.0f, 11.0f, 8.0f, 2.0f, 1.0f, 8.0f
    };
    inline constexpr float kPcpH[13] = {
        0.28f, 0.25f, 0.25f, 0.24f, 0.22f, 0.24f, 0.20f,
        0.20f, 0.18f, 0.21f, 0.32f, 0.49f, 0.51f
    };

    inline float Smooth01(float x) noexcept
    {
        if (!std::isfinite(x))
            return 0.0f;
        const float u = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
        return u * u * (3.0f - 2.0f * u);
    }

    inline float PcpAt(const float* table, float seconds) noexcept
    {
        const float x = seconds / kPcpDt;
        if (!std::isfinite(x) || x <= 0.0f)
            return table[0];
        if (x >= 12.0f)
            return table[12];
        const int i = static_cast<int>(x);
        const float f = x - static_cast<float>(i);
        return table[i] + (table[i + 1] - table[i]) * f;
    }

    struct SpPose {
        float pos[3] = { 0.0f, 0.0f, 0.0f };
        float yaw = 0.0f;      // +Z-forward, radians
        float pitch = 0.0f;
        float fov = 70.0f;
    };

    // `role` 1 = attacker, 2 = victim. `me` is that screen's own body root,
    // `other` the partner's root. The attacker's framing uses ext v2's 1.55 +
    // 0.35 * gap distance; the victim's uses the authored PCP_D dolly.
    inline SpPose SpCamera(int role, float seconds, const float me[3],
                           const float other[3], float lift = 0.0f) noexcept
    {
        SpPose out;
        if (!me || !other ||
            !std::isfinite(me[0] + me[1] + me[2] + other[0] + other[1] + other[2]))
            return out;
        out.pos[0] = me[0];
        out.pos[1] = me[1] + 1.5f;
        out.pos[2] = me[2];

        const bool attacker = role == 1;
        const float t = seconds > 0.0f ? seconds : 0.0f;
        const float dx = other[0] - me[0], dz = other[2] - me[2];
        const float gap = std::min(2.5f, std::sqrt(dx * dx + dz * dz));
        const float dolly = PcpAt(kPcpD, t);
        const float push = dolly / kPcpD[0];
        const float D = attacker ? (1.55f + 0.35f * gap) * push : dolly;
        const float offDeg = PcpAt(kPcpOff, t) + (attacker ? 32.0f : 18.0f);
        const float H = me[1] + (attacker ? 1.7f : PcpAt(kPcpH, t)) + lift;

        const float behind = std::atan2(-dx, -dz);
        const float a = behind + offDeg * 3.14159265f / 180.0f;
        out.pos[0] = me[0] + std::sin(a) * D;
        out.pos[1] = H;
        out.pos[2] = me[2] + std::cos(a) * D;

        const float blend = attacker ? 0.65f : 0.40f;
        const float aim[3] = {
            me[0] + dx * blend,
            me[1] + (attacker ? 1.20f : 0.50f) + (other[1] - me[1]) * blend,
            me[2] + dz * blend
        };
        const float ax = aim[0] - out.pos[0], az = aim[2] - out.pos[2];
        const float flat = std::sqrt(ax * ax + az * az);
        out.yaw = std::atan2(ax, az);
        out.pitch = std::atan2(aim[1] - out.pos[1], flat > 1e-4f ? flat : 1e-4f);
        out.fov = 70.0f + 3.7f * Smooth01(t / kSpSwingSecs);
        return out;
    }

    // ---- paired placement ------------------------------------------------------------
    // The authored apReference channels live in the private psync data and are
    // not in this build. Both screens can still agree on a deterministic root
    // pair: the victim stays at its own root, the attacker stands at the feet
    // side (behind the victim's facing yaw) and faces into the drag. The clips
    // published by paired:: do the posing; this only places the two roots.
    struct PairPose {
        float attPos[3] = { 0.0f, 0.0f, 0.0f };
        float vicPos[3] = { 0.0f, 0.0f, 0.0f };
        float attYaw = 0.0f;
        float vicYaw = 0.0f;
    };

    inline PairPose PulloutPairPose(const float vicRoot[3], float yaw,
                                     float gap = 0.95f) noexcept
    {
        PairPose out;
        if (!vicRoot || !std::isfinite(vicRoot[0] + vicRoot[1] + vicRoot[2] + yaw))
            return out;
        out.vicPos[0] = vicRoot[0];
        out.vicPos[1] = vicRoot[1];
        out.vicPos[2] = vicRoot[2];
        out.vicYaw = yaw;
        out.attYaw = yaw;
        out.attPos[0] = vicRoot[0] - std::sin(yaw) * gap;
        out.attPos[1] = vicRoot[1];
        out.attPos[2] = vicRoot[2] - std::cos(yaw) * gap;
        return out;
    }

} // namespace gameplay::melee_pullout::core
