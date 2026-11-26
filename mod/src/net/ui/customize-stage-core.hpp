#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Pure layout / tuning / timing rules for tools/ext/customize_stage.c (build
// 191). The native owner in customize-stage.cpp owns the engine effects; every
// decision that can be checked without an engine stays here and is tested by
// mod/tests/customize_stage_core_test.cpp.

namespace net::ui::customize::stage::core {

    inline constexpr int      kScenes = 2;          // menu scene 1 WLF, 2 Seraphites
    inline constexpr uint64_t kFilePollMs = 1000;
    inline constexpr uint64_t kCamReadyMs = 180;    // camera cut under the fade
    inline constexpr int      kSwapMsDef = 800;
    inline constexpr int      kSwapMsMin = 100;
    inline constexpr int      kSwapMsMax = 5000;
    inline constexpr float    kFrontD = 4.24f;
    inline constexpr float    kFrontX = 0.586f;
    inline constexpr float    kFrontY = 0.636f;
    inline constexpr float    kBackD = 5.94f;
    inline constexpr float    kBackX = 2.92f;
    inline constexpr float    kSwapBlendMs = 500.0f;
    inline constexpr int      kRiseMs = 320;
    inline constexpr float    kHideY = 40.0f;
    inline constexpr float    kRad = 3.14159265f / 180.0f;
    inline constexpr float    kLightDef = 0.13f;
    inline constexpr float    kLightStep = 1.10f;
    inline constexpr float    kLightMin = 0.01f;
    inline constexpr float    kLightMax = 4.0f;
    inline constexpr int      kCineCount = 5;
    inline constexpr int      kCineDef = 3;         // C OVERCAST SOFTBOX on both stages

    // Model lifecycle / freecam / input timing from tools/ext/customize_stage.c.
    inline constexpr uint64_t kPreviewHoldMs = 220;     // focused row must rest before previewing
    inline constexpr uint64_t kMinLifeMs = 1200;       // a standing model is not replaced faster
    inline constexpr uint64_t kUnarmMs = 120;
    inline constexpr uint64_t kSpawnGapMs = 500;
    inline constexpr float    kFaceSecs = 4.0f;
    inline constexpr uint64_t kFaceEveryMs = 2000;
    inline constexpr uint64_t kCleanEveryMs = 1000;
    inline constexpr uint64_t kEyesDelayMs = 400;
    inline constexpr uint64_t kEyesEveryMs = 1000;
    inline constexpr float    kEyesSecs = 3.0f;
    inline constexpr float    kEyesDeg = 55.0f;
    inline constexpr float    kPoseDelayMs = 150.0f;
    inline constexpr float    kSwapBlendSecs = 0.5f;
    inline constexpr int      kGraveN = 20;
    inline constexpr int      kGraveRoom = 6;
    inline constexpr int      kPoolKeep = 10;
    inline constexpr uint64_t kKillAgeMs = 5000;
    inline constexpr uint64_t kKillQuietMs = 2000;
    inline constexpr uint64_t kKillGapMs = 800;
    inline constexpr int      kKillQ = 12;
    inline constexpr int      kKillClearFrames = 4;
    inline constexpr int      kHoldFrames = 6;
    inline constexpr float    kFreecamLookPx = 0.12f;
    inline constexpr float    kFreecamFovBase = 0.45f;
    inline constexpr float    kFreecamFovMin = 5.0f;
    inline constexpr float    kFreecamFovMax = 120.0f;

    // tools/ext/customize_stage_layout.h (build 191). Empty scenes use the
    // derived layout; the native owner parses these lines once and lets
    // t2r-cz-stage.txt override them per scene.
    inline constexpr const char* kShippedLayout =
        "model 1 0 -36.219 -8.994 1042.823 -177.86\n"
        "model 1 1 -38.389 -8.990 1046.211 176.20\n"
        "camera 1 -35.613 -7.829 1036.500 -1.80 0.60 40.00\n"
        "wlight 1 0 -36.755 -7.348 1038.831 -36.043 -7.878 1042.823 1.000 0.900 0.780 6.00 40.0 1\n"
        "wlight 1 1 -35.683 -7.098 1046.815 -36.219 -7.494 1042.823 0.700 0.820 1.000 8.00 35.0 0\n"
        "wlight 1 2 0 0 0 0 0 0 0 0 0 0 0 0\n"
        "spose 1 0 npc-mm-ambi-buddy-idle\n"
        "spose 1 1 cap-npc-m-ambi-idle-lean-wall-loop 0.031 0.000 -0.404 0.00\n"
        "model 2 0 280.207 20.064 61.801 -65.60\n"
        "model 2 1 282.777 20.095 62.427 -71.10\n"
        "camera 2 274.204 21.594 64.607 120.00 4.20 40.00\n"
        "wlight 2 0 278.218 21.913 64.311 280.340 21.314 61.022 1.000 0.900 0.780 2.35 40.0 1\n"
        "wlight 2 1 282.195 22.163 59.290 280.207 21.564 61.801 0.700 0.820 1.000 8.00 35.0 0\n"
        "wlight 2 2 0 0 0 0 0 0 0 0 0 0 0 0\n"
        "spose 2 0 npc-mm-ambi-buddy-idle\n"
        "spose 2 1 efj-lounging-idle--male-b -1.200 -0.061 5.393 -147.06\n";

    struct CineLight {
        float az, el, d;
        float r, g, b;
        float i, cone, sh, soft;
    };

    struct Cine {
        const char* name;
        CineLight l[3];
    };

    // ext CZS_CINE (build 191), in setup order A..E. Setup 0 is the layout's
    // own wlight lines, not an entry in this table.
    inline constexpr Cine kCine[kCineCount] = {
        {"A  BACKLIT SUN", {
            {-40, 35, 3.0f, 1.00f, 0.90f, 0.78f, 0.80f, 45, 1, 0.5f},
            {150, 40, 3.5f, 1.00f, 0.93f, 0.82f, 2.00f, 35, 0, 0},
            {55, 10, 3.5f, 0.70f, 0.82f, 1.00f, 0.20f, 60, 0, 0},
        }},
        {"B  SUN KEY", {
            {40, 45, 3.5f, 1.00f, 0.88f, 0.72f, 1.10f, 45, 1, 0},
            {-150, 35, 3.5f, 0.75f, 0.85f, 1.00f, 1.50f, 35, 0, 0},
            {-50, 5, 3.5f, 0.65f, 0.78f, 1.00f, 0.18f, 60, 0, 0},
        }},
        {"C  OVERCAST SOFTBOX", {
            {-15, 50, 3.0f, 0.86f, 0.92f, 1.00f, 0.70f, 70, 1, 1.0f},
            {-160, 45, 3.5f, 0.70f, 0.85f, 1.00f, 1.80f, 35, 0, 0},
            {100, -5, 2.5f, 1.00f, 0.72f, 0.45f, 0.40f, 50, 0, 0},
        }},
        {"D  LOW-KEY", {
            {70, 30, 3.0f, 1.00f, 0.96f, 0.90f, 1.30f, 35, 1, 0},
            {-140, 40, 3.5f, 0.85f, 0.90f, 1.00f, 1.60f, 35, 0, 0},
            {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        }},
        {"E  STUDIO", {
            {-30, 35, 3.0f, 1.00f, 0.97f, 0.94f, 0.80f, 60, 1, 0.8f},
            {180, 50, 3.5f, 1.00f, 1.00f, 1.00f, 0.80f, 40, 0, 0},
            {40, 15, 3.5f, 0.95f, 0.97f, 1.00f, 0.45f, 60, 0, 0.8f},
        }},
    };

    struct Tuning {
        float spot[3][4] = {{0, 0, 0, 999}, {0, 0, 0, 999}, {0, 0, 0, 999}};
        float fov = 40.0f, pitch = 2.0f;
        int   swapMs = kSwapMsDef, swapDir = 1, swapMode = 1;
        bool  lights = true, eyes = true;
        float poseYaw = 0.0f;
        char  face[24] = "neutral";
        char  cls[2][32] = {"Pirate", "Pirate"};
        char  arch[2][40] = {"militia-pistol-light", "scar-light"};
        char  look[2][40] = {"", ""};
        char  fpose[2][2][48] = {{"", ""}, {"", ""}};
        char  spose[3][2][48] = {
            {{}, {}},
            {{"npc-mm-ambi-buddy-idle"}, {"cap-npc-m-ambi-idle-lean-wall-loop"}},
            {{"npc-mm-ambi-buddy-idle"}, {"efj-lounging-idle--male-b"}},
        };
        float sapo[3][2][4] = {
            {{0, 0, 0, 0}, {0, 0, 0, 0}},
            {{0, 0, 0, 0}, {0.031f, 0.0f, -0.404f, 0.0f}},
            {{0, 0, 0, 0}, {-1.2f, -0.061f, 5.393f, -2.5674f}},
        };
        char  pose[2][4][40] = {
            {{"rogue-mode-rattler-factions"}, {"efj-lounging-idle--male-a"},
             {"gen-market-group-01-buyer-m"}, {}},
            {{"rogue-mode-scar-archer"}, {"npc-standing-ambi-idle"},
             {"efj-lounging-idle--female-a"}, {"gen-market-group-01-buyer-f"}},
        };
        float light[3][9] = {
            {-1.5f, 2.3f, 1.4f, 1.00f, 0.90f, 0.78f, 6.0f, 40.0f, 1},
            {3.4f, 2.1f, 7.6f, 0.70f, 0.82f, 1.00f, 8.0f, 35.0f, 0},
            {-0.6f, 2.6f, 3.6f, 1.00f, 0.92f, 0.82f, 3.0f, 30.0f, 0},
        };
    };

    // Scene Editor absolute layout lines (customize_stage_layout.h /
    // t2r-cz-stage.txt). Missing parts stay unset so the derived layout can
    // fill them, exactly like czs_abs_line() / czs_abs_on().
    struct Layout {
        int   scene = 0;              // the menu scene these absolute lines describe
        bool  model[2] = {false, false};
        bool  camera = false;
        bool  light[3] = {false, false, false};
        float mv[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
        float cv[6] = {0, 0, 0, 0, 0, 0};
        float lv[3][12] = {{0}, {0}, {0}};

        [[nodiscard]] bool Complete(int sc) const noexcept
        {
            return sc >= 1 && sc <= kScenes && scene == sc && model[0] && camera;
        }
    };

    struct LightSettings {
        float strength[2] = {kLightDef, kLightDef};
        int   setup[2] = {kCineDef, kCineDef};
        float setupK[2][kCineCount + 1] = {
            {0, 1, 1, 1, 1, 1},
            {0, 1, 1, 1, 1, 1},
        };
    };

    struct Derived {
        bool  valid = false;
        float cam[3] = {0, 0, 0};
        float yaw = 0.0f;             // radians, the view direction
        float front[3] = {0, 0, 0};   // feet
        float back[3] = {0, 0, 0};
        float frontRest = 0.0f;       // radians, facing at rest
        float backRest = 0.0f;
        float pitch = 0.0f, fov = 0.0f;
    };

    struct SwapFrame {
        float front[3] = {0, 0, 0};
        float back[3] = {0, 0, 0};
        bool  done = false;
    };

    // A world locator packed the way the engine's animate_ ap-reference expects:
    // position xyz + quaternion xyzw. apReference is relative to the stand
    // place's rotation, exactly like ext's CZS.sapo / czs_pose_play().
    [[nodiscard]] inline float WrapRadians(float a) noexcept
    {
        while (a > 3.14159265f) a -= 6.2831853f;
        while (a < -3.14159265f) a += 6.2831853f;
        return a;
    }

    inline void ApReference7(const float place[3], float yaw, const float apo[4],
                             float poseYawRad, float out[7]) noexcept
    {
        if (!place || !apo || !out) return;
        const float y = yaw + poseYawRad;
        const float c = std::cos(y), s = std::sin(y);
        out[0] = place[0] + apo[0] * c + apo[2] * s;
        out[1] = place[1] + apo[1];
        out[2] = place[2] - apo[0] * s + apo[2] * c;
        const float apYaw = y + apo[3];
        out[3] = 0.0f;
        out[4] = std::sin(apYaw * 0.5f);
        out[5] = 0.0f;
        out[6] = std::cos(apYaw * 0.5f);
    }

    // Camera quaternion: yaw(Y) * pitch(X). With +Z forward this produces the
    // ext's view vector (sin yaw cos pitch, -sin pitch, cos yaw cos pitch).
    inline void CameraQuat(float yawRad, float pitchRad, float out[4]) noexcept
    {
        if (!out) return;
        const float cy = std::cos(yawRad * 0.5f), sy = std::sin(yawRad * 0.5f);
        const float cp = std::cos(pitchRad * 0.5f), sp = std::sin(pitchRad * 0.5f);
        out[0] = cy * sp;
        out[1] = sy * cp;
        out[2] = -sy * sp;
        out[3] = cy * cp;
    }

    [[nodiscard]] inline float WrapDegrees(float a) noexcept
    {
        while (a > 180.0f) a -= 360.0f;
        while (a < -180.0f) a += 360.0f;
        return a;
    }

    [[nodiscard]] inline float ClampPitch(float pitch) noexcept
    {
        return std::clamp(pitch, -89.0f, 89.0f);
    }

    // Exponential fov zoom. `dir` is +1 for Z / RT (closer) and -1 for X / LT.
    [[nodiscard]] inline float FovZoom(float fov, int dir, float seconds) noexcept
    {
        const float dt = std::clamp(seconds, 0.0f, 0.1f);
        const float k = std::pow(kFreecamFovBase, dt);
        const float next = dir > 0 ? fov * k : fov / k;
        return std::clamp(next, kFreecamFovMin, kFreecamFovMax);
    }

    // The pose browser steps over only playable clips. -1 is "not picked yet";
    // from there forward starts at 0 and backward starts at the last entry.
    [[nodiscard]] inline int NextPoseIndex(int current, int count, int dir) noexcept
    {
        if (count <= 0) return -1;
        const int from = current < 0 ? (dir > 0 ? -1 : 0) : current;
        int i = (from + dir + count * 4) % count;
        if (i < 0) i += count;
        return i;
    }

    // Free-flight along the view and its right vector, with world-up on E / Q.
    // `speed` already includes Shift / Ctrl and the frame delta.
    inline void FreecamMove(float pos[3], float yawDeg, float pitchDeg,
                            float forward, float right, float up,
                            float speed) noexcept
    {
        if (!pos) return;
        const float y = yawDeg * kRad, p = pitchDeg * kRad;
        const float cp = std::cos(p);
        const float fx = std::sin(y) * cp, fz = std::cos(y) * cp;
        const float rx = -std::cos(y), rz = std::sin(y);
        pos[0] += (fx * forward + rx * right) * speed;
        pos[1] += (-std::sin(p) * forward + up) * speed;
        pos[2] += (fz * forward + rz * right) * speed;
    }

    // Inverse of LayoutFromSpot(): put the front feet back in front of a camera.
    inline void SpotFromCamera(const float cam[3], float yawDeg, float feetY,
                               float spot[4]) noexcept
    {
        if (!cam || !spot) return;
        const float y = yawDeg * kRad;
        const float fx = std::sin(y), fz = std::cos(y);
        const float rx = std::cos(y), rz = -std::sin(y);
        spot[0] = cam[0] + fx * kFrontD + rx * kFrontX;
        spot[1] = feetY;
        spot[2] = cam[2] + fz * kFrontD + rz * kFrontX;
        spot[3] = WrapDegrees(yawDeg);
    }

    // The menu scene is not exposed by a native; the current faction is the
    // closest available proxy, with the edited faction as the fallback.
    [[nodiscard]] inline int SceneFor(int currentSide, int editFac) noexcept
    {
        if (currentSide >= 0 && currentSide < kScenes) return currentSide + 1;
        if (editFac >= 0 && editFac < kScenes) return editFac + 1;
        return 0;
    }

    // One photo-mode runtime light slot, before the byte-level write.
    struct LightSlot {
        float pos[4] = {0, 0, 0, 1};
        float target[4] = {0, 0, 0, 1};
        float color[3] = {0, 0, 0};
        float intensity = 0.0f;
        float cone = 0.0f;
        float soft = 0.0f;
        bool  shadows = false;
        bool  enabled = false;
    };

    // Cinematic setup i around the front model's chest, relative to the
    // camera's ground-plane direction. Setup 0 is the layout's own lights.
    inline void CineSlots(int setup, const float front[3], const float cam[3],
                          float strength, LightSlot out[3]) noexcept
    {
        if (!out) return;
        for (int i = 0; i < 3; ++i) out[i] = LightSlot{};
        if (setup < 1 || setup > kCineCount || !front || !cam) return;
        const float chest[3] = {front[0], front[1] + 1.35f, front[2]};
        float hx = cam[0] - front[0], hz = cam[2] - front[2];
        float hl = std::sqrt(hx * hx + hz * hz);
        if (hl < 0.01f) { hx = front[0] - cam[0]; hz = front[2] - cam[2]; hl = 1.0f; }
        hx /= hl; hz /= hl;
        for (int i = 0; i < 3; ++i) {
            const CineLight& p = kCine[setup - 1].l[i];
            LightSlot& q = out[i];
            q = LightSlot{};
            if (p.i <= 0.0f) continue;
            const float az = p.az * kRad, el = p.el * kRad;
            const float dx = hx * std::cos(az) - hz * std::sin(az);
            const float dz = hz * std::cos(az) + hx * std::sin(az);
            q.pos[0] = chest[0] + p.d * std::cos(el) * dx;
            q.pos[1] = chest[1] + p.d * std::sin(el);
            q.pos[2] = chest[2] + p.d * std::cos(el) * dz;
            q.target[0] = chest[0];
            q.target[1] = chest[1] + (i == 1 ? 0.15f : 0.0f);
            q.target[2] = chest[2];
            q.color[0] = p.r; q.color[1] = p.g; q.color[2] = p.b;
            q.intensity = p.i * strength;
            q.cone = p.cone;
            q.soft = p.soft;
            q.shadows = p.sh > 0.5f;
            q.enabled = true;
        }
    }

    // The layout's three world lights. Slot 2 stays dark; slot 1 mirrors the
    // key across the front chest when the Scene Editor gave a key line.
    inline void LayoutSlots(const Layout& layout, int scene, const float cam[3],
                            const float front[3], const float back[3],
                            const float relative[3][9], float strength,
                            LightSlot out[3]) noexcept
    {
        if (!cam || !front || !back || !relative || !out) return;
        const bool abs = layout.Complete(scene);
        const float yaw = abs ? layout.cv[3] * kRad : 0.0f;
        const float fx = std::sin(yaw), fz = std::cos(yaw);
        const float rx = std::cos(yaw), rz = -std::sin(yaw);
        const float chest[3] = {front[0], front[1] + 1.35f, front[2]};
        for (int i = 0; i < 3; ++i) {
            out[i] = LightSlot{};
            if (i == 2) continue;
            const float* aim = i == 0 ? front : back;
            const float* l = relative[i];
            if (abs && i == 1) {
                if (!layout.light[0]) continue;
                const float* k = layout.lv[0];
                out[i].pos[0] = 2.0f * chest[0] - k[0];
                out[i].pos[1] = k[1] + 0.25f;
                out[i].pos[2] = 2.0f * chest[2] - k[2];
                out[i].target[0] = chest[0];
                out[i].target[1] = chest[1] + 0.15f;
                out[i].target[2] = chest[2];
                if (layout.light[1] && layout.lv[1][9] > 0.0f) l = layout.lv[1] + 3;
            } else if (abs) {
                if (!layout.light[i]) continue;
                const float* w = layout.lv[i];
                out[i].pos[0] = w[0]; out[i].pos[1] = w[1]; out[i].pos[2] = w[2];
                out[i].target[0] = w[3]; out[i].target[1] = w[4]; out[i].target[2] = w[5];
                l = w + 3;
            } else {
                out[i].pos[0] = cam[0] + rx * l[0] + fx * l[2];
                out[i].pos[1] = aim[1] + l[1];
                out[i].pos[2] = cam[2] + rz * l[0] + fz * l[2];
                out[i].target[0] = aim[0];
                out[i].target[1] = aim[1] + 1.25f;
                out[i].target[2] = aim[2];
            }
            out[i].color[0] = l[3]; out[i].color[1] = l[4]; out[i].color[2] = l[5];
            out[i].intensity = l[6] * strength;
            out[i].cone = l[7];
            out[i].soft = 0.0f;
            out[i].shadows = l[8] > 0.5f;
            out[i].enabled = l[6] > 0.0f;
        }
    }

    // Graveyard / kill queue decisions. The ext holds removals back until the
    // stage has been quiet from spawns and earlier removals.
    [[nodiscard]] inline bool CanSpawn(uint64_t now, uint64_t lastSpawn,
                                       bool killPending, uint64_t framesSinceKill) noexcept
    {
        return !killPending && framesSinceKill > static_cast<uint64_t>(kKillClearFrames)
            && (lastSpawn == 0 || now >= lastSpawn + kSpawnGapMs);
    }

    [[nodiscard]] inline bool CanRetire(uint64_t now, uint64_t spawnAt,
                                        uint64_t lastSpawn, uint64_t lastKill,
                                        bool killPending, int parked,
                                        bool pageUp) noexcept
    {
        const int keep = pageUp ? kPoolKeep : 0;
        return now >= spawnAt + kKillAgeMs && parked > keep && !killPending
            && (lastSpawn == 0 || now - lastSpawn >= kKillQuietMs)
            && (lastKill == 0 || now - lastKill >= kKillGapMs);
    }

    [[nodiscard]] inline bool SceneForFactionIsValid(int scene) noexcept
    {
        return scene >= 1 && scene <= kScenes;
    }

    [[nodiscard]] inline int ClampSwapMs(int ms) noexcept
    {
        return std::clamp(ms, kSwapMsMin, kSwapMsMax);
    }

    [[nodiscard]] inline int SwapDir(int dir) noexcept { return dir < 0 ? -1 : 1; }

    // ext czs_ease(): smoothstep, clamped before the cubic.
    [[nodiscard]] inline float Ease(float t) noexcept
    {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    [[nodiscard]] inline float PercentToStrength(float percent) noexcept
    {
        return std::clamp(percent / 100.0f, kLightMin, kLightMax);
    }

    [[nodiscard]] inline float StepStrength(float k, int dir) noexcept
    {
        return std::clamp(dir > 0 ? k * kLightStep : k / kLightStep,
                          kLightMin, kLightMax);
    }

    [[nodiscard]] inline int NextSetup(int setup) noexcept
    {
        return (setup + 1) % (kCineCount + 1);
    }

    [[nodiscard]] inline const char* SetupName(int setup) noexcept
    {
        return setup >= 1 && setup <= kCineCount ? kCine[setup - 1].name : "YOUR LAYOUT";
    }

    [[nodiscard]] inline float ActiveStrength(const LightSettings& s, int stage) noexcept
    {
        if (stage != 0 && stage != 1) return kLightDef;
        const int n = s.setup[stage];
        return n >= 0 && n <= kCineCount ? s.setupK[stage][n] : kLightDef;
    }

    // ext czs_face(): atan2(camera - model), radians; 0 = +Z toward the camera.
    [[nodiscard]] inline float FaceYaw(const float from[3], const float cam[3]) noexcept
    {
        if (!from || !cam) return 0.0f;
        return std::atan2(cam[0] - from[0], cam[2] - from[2]);
    }

    // ext czs_layout(): camera behind / above the front feet, back model along
    // the view line and to the right. forward = (sin yaw, 0, cos yaw).
    inline void LayoutFromSpot(const float spot[4], float cam[3],
                               float front[3], float back[3]) noexcept
    {
        if (!spot || !cam || !front || !back) return;
        const float yaw = spot[3] * kRad;
        const float fx = std::sin(yaw), fz = std::cos(yaw);
        const float rx = std::cos(yaw), rz = -std::sin(yaw);
        cam[0] = spot[0] - fx * kFrontD - rx * kFrontX;
        cam[1] = spot[1] + kFrontY;
        cam[2] = spot[2] - fz * kFrontD - rz * kFrontX;
        front[0] = spot[0]; front[1] = spot[1]; front[2] = spot[2];
        back[0] = spot[0] + fx * (kBackD - kFrontD) + rx * (kBackX - kFrontX);
        back[1] = spot[1];
        back[2] = spot[2] + fz * (kBackD - kFrontD) + rz * (kBackX - kFrontX);
    }

    // The absolute layout wins per scene; otherwise a configured spot is used.
    // The engine's hidden menu player locator is unavailable natively, so an
    // unset spot leaves the stage inert instead of deriving one.
    [[nodiscard]] inline Derived MakeDerived(const Layout& layout, int scene,
                                             const Tuning& t) noexcept
    {
        Derived d;
        if (scene < 1 || scene > kScenes) return d;
        if (layout.Complete(scene)) {
            d.valid = true;
            for (int k = 0; k < 3; ++k) {
                d.cam[k] = layout.cv[k];
                d.front[k] = layout.mv[0][k];
            }
            d.yaw = layout.cv[3] * kRad;
            d.pitch = layout.cv[4];
            d.fov = layout.cv[5];
            d.frontRest = layout.mv[0][3] * kRad;
            if (layout.model[1]) {
                for (int k = 0; k < 3; ++k) d.back[k] = layout.mv[1][k];
                d.backRest = layout.mv[1][3] * kRad;
            } else if (t.spot[scene][3] < 900.0f) {
                // ext keeps the derived back place when the editor gave only
                // the front model and camera; that model still faces the camera.
                float spotCam[3]{}, spotFront[3], spotBack[3];
                LayoutFromSpot(t.spot[scene], spotCam, spotFront, spotBack);
                for (int k = 0; k < 3; ++k) d.back[k] = spotBack[k];
                d.backRest = FaceYaw(spotBack, d.cam);
            } else {
                d.backRest = d.yaw;
            }
            return d;
        }
        if (t.spot[scene][3] >= 900.0f) return d;
        float cam[3]{}, front[3]{}, back[3]{};
        LayoutFromSpot(t.spot[scene], cam, front, back);
        if (!layout.model[1]) {
            // ext keeps the derived back place when the editor only gave the
            // front model and camera.
            for (int k = 0; k < 3; ++k) d.back[k] = back[k];
        }
        d.valid = true;
        for (int k = 0; k < 3; ++k) {
            d.cam[k] = cam[k];
            d.front[k] = front[k];
        }
        d.yaw = t.spot[scene][3] * kRad;
        d.pitch = t.pitch;
        d.fov = t.fov;
        d.frontRest = FaceYaw(d.front, d.cam);
        d.backRest = FaceYaw(d.back, d.cam);
        return d;
    }

    // ext czs_tick()'s two-circle exchange. The incoming model rises on its
    // old clip for CZS_RISE_MS before it starts moving; both arrive together.
    [[nodiscard]] inline SwapFrame SwapPositions(const float fp[3], const float bp[3],
                                                 uint64_t elapsedMs, int swapMs,
                                                 int dir, bool rise) noexcept
    {
        SwapFrame f;
        if (!fp || !bp) return f;
        const int ms = ClampSwapMs(swapMs);
        const uint64_t el = std::min<uint64_t>(elapsedMs, static_cast<uint64_t>(ms));
        if (elapsedMs >= static_cast<uint64_t>(ms)) {
            // ext snaps to the destination SLOTS (front <- fp, back <- bp) and
            // flips the displayed faction only after the circle is complete.
            for (int k = 0; k < 3; ++k) { f.front[k] = fp[k]; f.back[k] = bp[k]; }
            f.done = true;
            return f;
        }
        const uint64_t riseMs = rise ? static_cast<uint64_t>(kRiseMs) : 0;
        const float t = Ease(static_cast<float>(el) / static_cast<float>(ms));
        const uint64_t denom = static_cast<uint64_t>(ms) > riseMs + 100
                             ? static_cast<uint64_t>(ms) - riseMs : 100;
        const float t2 = el <= riseMs ? 0.0f
            : Ease(static_cast<float>(el - riseMs) / static_cast<float>(denom));
        const float mx = (fp[0] + bp[0]) * 0.5f, mz = (fp[2] + bp[2]) * 0.5f;
        const float ox = fp[0] - mx, oz = fp[2] - mz;
        const float a = -static_cast<float>(SwapDir(dir)) * 3.14159265f * t;
        const float c = std::cos(a), s = std::sin(a);
        f.front[0] = mx + ox * c + oz * s;
        f.front[2] = mz - ox * s + oz * c;
        f.front[1] = fp[1] + (bp[1] - fp[1]) * t;
        const float a2 = -static_cast<float>(SwapDir(dir)) * 3.14159265f * t2;
        const float c2 = std::cos(a2), s2 = std::sin(a2);
        f.back[0] = mx - ox * c2 - oz * s2;
        f.back[2] = mz + ox * s2 - oz * c2;
        f.back[1] = bp[1] + (fp[1] - bp[1]) * t2;
        return f;
    }

    namespace detail {

        inline void Copy(char* dst, size_t cap, const char* src) noexcept
        {
            if (!dst || cap == 0) return;
            if (!src || std::strcmp(src, "-") == 0) { dst[0] = 0; return; }
            std::snprintf(dst, cap, "%s", src);
        }

        inline bool ParseFpose(const char* ln, Tuning& t) noexcept
        {
            int fc = 0, sx = 0;
            char c[48];
            if (std::sscanf(ln, "fpose %d %d %47s", &fc, &sx, c) != 3
                || fc < 0 || fc > 1 || sx < 0 || sx > 1) return false;
            Copy(t.fpose[fc][sx], sizeof t.fpose[fc][sx], c);
            return true;
        }

        inline bool ParseSpose(const char* ln, Tuning& t) noexcept
        {
            int sc = 0, sl = 0;
            char c[48];
            float a[4] = {0, 0, 0, 0};
            const int n = std::sscanf(ln, "spose %d %d %47s %f %f %f %f",
                                      &sc, &sl, c, &a[0], &a[1], &a[2], &a[3]);
            if ((n != 3 && n != 7) || sc < 1 || sc > kScenes || sl < 0 || sl > 1)
                return false;
            Copy(t.spose[sc][sl], sizeof t.spose[sc][sl], c);
            a[3] *= kRad;
            for (int k = 0; k < 4; ++k) t.sapo[sc][sl][k] = a[k];
            return true;
        }

        inline bool ParsePoseList(const char* ln, Tuning& t) noexcept
        {
            if (std::strncmp(ln, "pose ", 5) != 0 || (ln[5] != '0' && ln[5] != '1'))
                return false;
            const int sx = ln[5] - '0';
            char buf[192];
            std::snprintf(buf, sizeof buf, "%s", ln + 6);
            for (int k = 0; k < 4; ++k) t.pose[sx][k][0] = 0;
            int n = 0;
            for (char* w = std::strtok(buf, " \t\r\n"); w && n < 4;
                 w = std::strtok(nullptr, " \t\r\n"))
                if (std::strcmp(w, "-") != 0) Copy(t.pose[sx][n++], 40, w);
            return true;
        }

    } // namespace detail

    // One t2r-cz-stage.txt line. Absolute model/camera/wlight lines are parsed
    // by ParseLayoutLine() into a separate Layout first, like ext's reader.
    inline bool ParseStageLine(const char* ln, Tuning& t) noexcept
    {
        if (!ln || !*ln) return false;
        if (detail::ParseFpose(ln, t) || detail::ParseSpose(ln, t)
            || detail::ParsePoseList(ln, t)) return true;

        int sc = 0, fc = 0, ms = 0, dir = 0;
        float v[4] = {0, 0, 0, 0}, w[9] = {0};
        char a[40] = "", b[40] = "";
        if (std::sscanf(ln, "spot %d %f %f %f %f", &sc, &v[0], &v[1], &v[2], &v[3]) == 5
            && sc >= 0 && sc < 3) {
            for (int k = 0; k < 4; ++k) t.spot[sc][k] = v[k];
            return true;
        }
        if (std::sscanf(ln, "cam %f %f", &v[0], &v[1]) == 2
            && v[0] > 5.0f && v[0] < 120.0f) {
            t.fov = v[0]; t.pitch = v[1];
            return true;
        }
        if (std::sscanf(ln, "swap %d %d", &ms, &dir) == 2) {
            t.swapMs = ClampSwapMs(ms);
            t.swapDir = SwapDir(dir);
            return true;
        }
        if (std::sscanf(ln, "face %23s", a) == 1) { detail::Copy(t.face, sizeof t.face, a); return true; }
        if (std::sscanf(ln, "eyes %d", &fc) == 1) { t.eyes = fc != 0; return true; }
        if (std::sscanf(ln, "lights %d", &fc) == 1) { t.lights = fc != 0; return true; }
        if (std::sscanf(ln, "poseyaw %f", &v[0]) == 1) { t.poseYaw = v[0]; return true; }
        if (std::sscanf(ln, "swapmode %d", &fc) == 1) { t.swapMode = fc != 0; return true; }
        if (std::sscanf(ln, "npc %d %39s %39s", &fc, a, b) == 3 && (fc == 0 || fc == 1)) {
            detail::Copy(t.cls[fc], sizeof t.cls[fc], a);
            detail::Copy(t.arch[fc], sizeof t.arch[fc], b);
            return true;
        }
        if (std::sscanf(ln, "look %d %39s", &fc, a) == 2 && (fc == 0 || fc == 1)) {
            detail::Copy(t.look[fc], sizeof t.look[fc], a);
            return true;
        }
        if (std::sscanf(ln, "light %d %f %f %f %f %f %f %f %f %f", &fc,
                        &w[0], &w[1], &w[2], &w[3], &w[4], &w[5], &w[6], &w[7], &w[8]) == 10
            && fc >= 0 && fc < 3) {
            for (int k = 0; k < 9; ++k) t.light[fc][k] = w[k];
            return true;
        }
        return false;
    }

    inline bool ParseLayoutLine(const char* ln, Layout& layout) noexcept
    {
        if (!ln || !*ln) return false;
        int sc = 0, k = 0;
        float v[12] = {0};
        if (std::sscanf(ln, "model %d %d %f %f %f %f", &sc, &k,
                        &v[0], &v[1], &v[2], &v[3]) == 6
            && sc >= 1 && sc <= kScenes && (k == 0 || k == 1)) {
            for (int i = 0; i < 4; ++i) layout.mv[k][i] = v[i];
            layout.scene = sc;
            layout.model[k] = true;
            return true;
        }
        if (std::sscanf(ln, "camera %d %f %f %f %f %f %f", &sc,
                        &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 7
            && sc >= 1 && sc <= kScenes && v[5] > 5.0f && v[5] < 120.0f) {
            for (int i = 0; i < 6; ++i) layout.cv[i] = v[i];
            layout.scene = sc;
            layout.camera = true;
            return true;
        }
        if (std::sscanf(ln, "wlight %d %d %f %f %f %f %f %f %f %f %f %f %f %f", &sc, &k,
                        &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                        &v[8], &v[9], &v[10], &v[11]) == 14
            && sc >= 1 && sc <= kScenes && k >= 0 && k < 3) {
            for (int i = 0; i < 12; ++i) layout.lv[k][i] = v[i];
            layout.scene = sc;
            layout.light[k] = true;
            return true;
        }
        return false;
    }

    // One t2r-cz-light.txt line. The owner writes the same format back when L
    // or [ / ] changes the stage lights during a visit.
    inline bool ParseLightLine(const char* ln, LightSettings& s) noexcept
    {
        if (!ln || !*ln) return false;
        char key[16] = "";
        float v = 0.0f, w = 0.0f;
        const int n = std::sscanf(ln, "%15s %f %f", key, &v, &w);
        if (n == 3 && (!std::strcmp(key, "wlf-setup") || !std::strcmp(key, "scar-setup"))) {
            const int stage = key[0] == 's' ? 1 : 0;
            const int setup = static_cast<int>(v);
            if (setup < 0 || setup > kCineCount) return false;
            s.setup[stage] = setup;
            if (setup && w >= 1.0f && w <= 400.0f)
                s.setupK[stage][setup] = PercentToStrength(w);
            return true;
        }
        if (n == 2 && v >= 1.0f && v <= 400.0f) {
            if (!std::strcmp(key, "wlf")) s.strength[0] = PercentToStrength(v);
            else if (!std::strcmp(key, "scar")) s.strength[1] = PercentToStrength(v);
            else return false;
            return true;
        }
        if (std::sscanf(ln, "%f", &v) == 1 && v >= 1.0f && v <= 400.0f) {
            s.strength[0] = s.strength[1] = PercentToStrength(v);
            return true;
        }
        return false;
    }

    // Exact t2r-cz-light.txt write format; setup 0 prints 100 % because setup
    // strength has no meaning until a cinematic setup is selected.
    inline void FormatLightFile(const LightSettings& s, char* out, size_t cap) noexcept
    {
        if (!out || cap == 0) return;
        std::snprintf(out, cap,
                      "wlf %.1f\nscar %.1f\n"
                      "wlf-setup %d %.1f\n"
                      "scar-setup %d %.1f\n",
                      s.strength[0] * 100.0f, s.strength[1] * 100.0f,
                      s.setup[0], (s.setup[0] ? s.setupK[0][s.setup[0]] : 1.0f) * 100.0f,
                      s.setup[1], (s.setup[1] ? s.setupK[1][s.setup[1]] : 1.0f) * 100.0f);
    }

    // t2r-cz-stage-saved.txt lines written by the freecam F9 / F10 helpers.
    inline void FormatStageSpot(int scene, const float spot[4], float fov, float pitch,
                                char* out, size_t cap) noexcept
    {
        if (!out || cap == 0 || !spot || !SceneForFactionIsValid(scene)) { if (out) out[0] = 0; return; }
        std::snprintf(out, cap, "spot %d %.3f %.3f %.3f %.2f\ncam %.1f %.1f\n",
                      scene, spot[0], spot[1], spot[2], spot[3], fov, pitch);
    }

} // namespace net::ui::customize::stage::core
