// Standalone harness for tools/ext/customize_stage.c layout / timing rules:
//   g++ -std=c++20 -Wall -Wextra -Werror -pedantic -I mod/src/net/ui
//       mod/tests/customize_stage_core_test.cpp -o /tmp/opencode/customize_stage_core_test
//   /tmp/opencode/customize_stage_core_test

#include "customize-stage-core.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

    using namespace net::ui::customize::stage::core;

    int fails = 0;

    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

    bool Near(float a, float b, float eps = 0.001f)
    {
        return std::fabs(a - b) <= eps;
    }

    bool Near3(const float a[3], float x, float y, float z, float eps = 0.001f)
    {
        return Near(a[0], x, eps) && Near(a[1], y, eps) && Near(a[2], z, eps);
    }

} // namespace

int main()
{
    std::printf("customize stage core:\n");

    Check(kSwapMsDef == 800 && kRiseMs == 320 && kSwapBlendMs == 500.0f
          && kHideY == 40.0f && kCineDef == 3,
          "swap / rise / blend / hide constants match build 191");
    Check(Near(kFrontD, 4.24f) && Near(kFrontX, 0.586f)
          && Near(kFrontY, 0.636f) && Near(kBackD, 5.94f)
          && Near(kBackX, 2.92f),
          "derived camera distances match the widget front / back areas");
    Check(ClampSwapMs(50) == 100 && ClampSwapMs(800) == 800
          && ClampSwapMs(9000) == 5000 && SwapDir(-9) == -1 && SwapDir(0) == 1,
          "swap time and direction clamp like the ext reader");

    Check(Ease(-1.0f) == 0.0f && Ease(0.0f) == 0.0f && Ease(1.0f) == 1.0f
          && Ease(2.0f) == 1.0f && Near(Ease(0.5f), 0.5f),
          "ease is clamped smoothstep");

    {
        const float spot[4] = {0, 0, 0, 90};
        float cam[3]{}, front[3]{}, back[3]{};
        LayoutFromSpot(spot, cam, front, back);
        Check(Near3(cam, -4.24f, 0.636f, 0.586f), "a 90 degree spot puts the camera behind and above");
        Check(Near3(front, 0, 0, 0), "the front model stands at the spot feet");
        Check(Near3(back, 1.70f, 0, -2.334f), "the back model is ahead and to the view's right");
        Check(Near(FaceYaw(front, cam), std::atan2(-4.24f, 0.586f)),
              "models face the camera with atan2(camera - model)");
        const float straight[3] = {0, 0, 4};
        Check(Near(FaceYaw(front, straight), 0.0f), "a camera on +Z needs zero facing");
    }

    {
        const float fp[3] = {0, 0, 0}, bp[3] = {2, 0, 0};
        const SwapFrame a = SwapPositions(fp, bp, 0, 800, 1, false);
        Check(!a.done && Near3(a.front, 0, 0, 0) && Near3(a.back, 2, 0, 0),
              "the exchange starts on the two stand places");
        const SwapFrame mid = SwapPositions(fp, bp, 400, 800, 1, false);
        Check(!mid.done && Near3(mid.front, 1, 0, -1) && Near3(mid.back, 1, 0, 1),
              "halfway both models are around the midpoint, clockwise from above");
        const SwapFrame done = SwapPositions(fp, bp, 800, 800, 1, false);
        Check(done.done && Near3(done.front, 0, 0, 0) && Near3(done.back, 2, 0, 0),
              "completion snaps to the destination slots and flips the front faction");
        const float highBack[3] = {2, 10, 0};
        const SwapFrame y = SwapPositions(fp, highBack, 400, 800, 1, false);
        Check(Near(y.front[1], 5.0f) && Near(y.back[1], 5.0f),
              "front / back heights exchange with the same ease");
        const SwapFrame rise = SwapPositions(fp, bp, 200, 800, 1, true);
        Check(Near3(rise.back, 2, 0, 0) && !Near3(rise.front, 0, 0, 0),
              "the incoming model waits out the rise before it moves");
        const SwapFrame still = SwapPositions(fp, bp, 6000, 9000, 1, false);
        Check(still.done, "an out-of-range swap duration clamps to 5000 ms");
    }

    {
        Tuning t{};
        Layout l{};
        Check(ParseLayoutLine("model 1 0 -36.219 -8.994 1042.823 -177.86", l)
              && l.model[0] && Near(l.mv[0][0], -36.219f),
              "model lines fill the absolute front / back place");
        Check(ParseLayoutLine("camera 1 -35.613 -7.829 1036.500 -1.80 0.60 40.00", l)
              && l.camera && Near(l.cv[5], 40.0f),
              "camera lines include yaw, pitch and fov");
        Check(ParseLayoutLine(
                  "wlight 1 0 -36.755 -7.348 1038.831 -36.043 -7.878 1042.823 "
                  "1.000 0.900 0.780 6.00 40.0 1", l)
              && l.light[0] && Near(l.lv[0][11], 1.0f),
              "wlight lines keep position, target and colour / cone / shadow");
        Check(l.Complete(1) && !l.Complete(0) && !l.Complete(2),
              "a scene is usable only with front model + camera");
        Check(!ParseLayoutLine("model 0 0 0 0 0 0", l)
              && !ParseLayoutLine("camera 1 0 0 0 0 0 200", l),
              "bad scene indexes and fovs are refused");

        const Derived d = MakeDerived(l, 1, t);
        Check(d.valid && Near(d.front[0], -36.219f) && Near(d.cam[0], -35.613f)
              && Near(d.pitch, 0.60f) && Near(d.fov, 40.0f),
              "the editor layout drives the derived stage camera");
        Check(!MakeDerived(l, 2, t).valid, "an unlaid-out scene has no fallback player locator");

        t.spot[1][3] = 90.0f;
        const Derived derived = MakeDerived(Layout{}, 1, t);
        Check(derived.valid && Near3(derived.cam, -4.24f, 0.636f, 0.586f)
              && Near(derived.fov, 40.0f),
              "a configured spot derives the same camera without the editor layout");
        const float faceDeg = 180.0f / 3.14159265f;
        Check(Near(derived.frontRest * faceDeg, -82.132f, 0.01f)
              && Near(derived.backRest * faceDeg, -63.822f, 0.01f),
              "derived models face the camera, not the spot's view yaw");
    }

    {
        Tuning t{};
        Check(ParseStageLine("spot 2 10 20 30 -45", t) && Near(t.spot[2][3], -45.0f),
              "spot lines accept scene 0..2 and degree yaw");
        Check(ParseStageLine("cam 35 4", t) && Near(t.fov, 35.0f) && Near(t.pitch, 4.0f),
              "cam lines set fov / pitch");
        Check(ParseStageLine("swap 50 -1", t) && t.swapMs == 100 && t.swapDir == -1,
              "swap lines clamp through the shared rule");
        Check(ParseStageLine("eyes 0", t) && !t.eyes
              && ParseStageLine("lights 0", t) && !t.lights
              && ParseStageLine("swapmode 0", t) && t.swapMode == 0,
              "boolean stage lines are read-only tuning");
        Check(ParseStageLine("npc 1 Pirate scar-light", t)
              && std::strcmp(t.arch[1], "scar-light") == 0,
              "npc lines keep the per-faction class / archetype");
        Check(ParseStageLine("look 0 t2r-cz-wm-5", t)
              && std::strcmp(t.look[0], "t2r-cz-wm-5") == 0,
              "forced look lines keep their record name");
        Check(ParseStageLine("fpose 1 0 rogue-mode-scar-archer", t)
              && std::strcmp(t.fpose[1][0], "rogue-mode-scar-archer") == 0,
              "fpose lines keep the Scene Editor pick");
        Check(ParseStageLine("spose 2 1 clip -1.2 -0.061 5.393 -147.06", t)
              && Near(t.sapo[2][1][3], -147.06f * kRad),
              "spose apReference yaw is converted from degrees");
        Check(ParseStageLine("light 1 1 2 3 4 5 6 7 8 9", t)
              && Near(t.light[1][8], 9.0f),
              "light lines keep the nine relative values");
        Check(!ParseStageLine("fpose 2 0 clip", t) && !ParseStageLine("npc 2 a b", t),
              "out-of-range faction / scene lines are refused");
    }

    {
        LightSettings s{};
        Check(ParseLightLine("wlf 13", s) && Near(s.strength[0], 0.13f),
              "wlf percent becomes the layout light strength");
        Check(ParseLightLine("scar-setup 3 25", s) && s.setup[1] == 3
              && Near(s.setupK[1][3], 0.25f),
              "setup lines select a cinematic setup and its strength");
        Check(ParseLightLine("50", s) && Near(s.strength[0], 0.50f)
              && Near(s.strength[1], 0.50f),
              "a bare percent counts for both stages");
        Check(!ParseLightLine("wlf 0", s) && !ParseLightLine("wlf-setup 9 50", s),
              "invalid percentages and setups are refused");
        Check(NextSetup(0) == 1 && NextSetup(kCineCount) == 0
              && std::strcmp(SetupName(0), "YOUR LAYOUT") == 0
              && std::strcmp(SetupName(3), "C  OVERCAST SOFTBOX") == 0,
              "L steps layout -> A..E -> layout");
        Check(Near(StepStrength(0.13f, 1), 0.13f * kLightStep)
              && StepStrength(kLightMax, 1) == kLightMax
              && StepStrength(kLightMin, -1) == kLightMin,
              "bracket step clamps to 1 % .. 400 %");
        Check(Near(ActiveStrength(s, 1), 0.25f),
              "the active stage reads its setup strength");
        char file[160]{};
        FormatLightFile(s, file, sizeof file);
        Check(std::strcmp(file, "wlf 50.0\nscar 50.0\nwlf-setup 3 100.0\nscar-setup 3 25.0\n") == 0,
              "the light sidecar is rewritten in the shipped four-line format");
    }

    {
        Check(NextPoseIndex(-1, 4, 1) == 0 && NextPoseIndex(-1, 4, -1) == 3
              && NextPoseIndex(1, 4, 1) == 2 && NextPoseIndex(0, 4, -1) == 3
              && NextPoseIndex(2, 0, 1) == -1,
              "the pose browser wraps over playable clips");
        char saved[192]{};
        const float spot[4] = {1.23456f, -2.5f, 3.0f, 91.234f};
        FormatStageSpot(2, spot, 40.0f, 2.0f, saved, sizeof saved);
        Check(std::strcmp(saved, "spot 2 1.235 -2.500 3.000 91.23\ncam 40.0 2.0\n") == 0,
              "the freecam writes the same spot / cam lines the stage reader parses");
    }

    {
        Check(SceneFor(1, 0) == 2 && SceneFor(-1, 0) == 1
              && SceneFor(-1, -1) == 0 && SceneFor(9, 1) == 2,
              "the current faction picks the menu scene, editing faction is the fallback");

        const float place[3] = {10, 2, -4};
        const float apo[4] = {0.031f, 0.0f, -0.404f, 0.0f};
        float ap[7]{};
        ApReference7(place, 0.0f, apo, 0.0f, ap);
        Check(Near(ap[0], 10.031f) && Near(ap[2], -4.404f)
              && Near(ap[3], 0.0f) && Near(ap[4], 0.0f) && Near(ap[6], 1.0f),
              "an apReference is rotated into the stand place and packed as a yaw quaternion");
        ApReference7(place, 1.5707963f, apo, 0.0f, ap);
        Check(Near(ap[0], 9.596f) && Near(ap[2], -4.031f),
              "a 90 degree stand yaw rotates the apReference offset around Y");

        float q[4]{};
        CameraQuat(0.0f, 0.0f, q);
        Check(Near(q[0], 0.0f) && Near(q[3], 1.0f), "an unpitched camera quaternion is identity");
        CameraQuat(1.5707963f, 0.0f, q);
        Check(Near(q[1], std::sin(0.78539816f)) && Near(q[3], std::cos(0.78539816f)),
              "yaw is a half-angle rotation about Y");
        CameraQuat(0.0f, 0.34906585f, q);
        Check(Near(q[0], std::sin(0.17453293f)) && Near(q[3], std::cos(0.17453293f)),
              "pitch is a half-angle rotation about X");
        CameraQuat(1.5707963f, 0.34906585f, q);
        // q * (0,0,1) * q^-1 for a yaw(Y) * pitch(X) quaternion.
        const float vx = 2.0f * (q[0] * q[2] + q[3] * q[1]);
        const float vy = 2.0f * (q[1] * q[2] - q[3] * q[0]);
        const float vz = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
        Check(Near(vx, std::cos(0.34906585f)) && Near(vy, -std::sin(0.34906585f))
              && Near(vz, 0.0f),
              "yaw * pitch looks along (sin yaw cos pitch, -sin pitch, cos yaw cos pitch)");

        Check(Near(WrapDegrees(181.0f), -179.0f) && Near(WrapDegrees(-181.0f), 179.0f)
              && ClampPitch(-100.0f) == -89.0f && ClampPitch(100.0f) == 89.0f,
              "freecam angles wrap at 180 and clamp pitch at 89 degrees");
        Check(Near(FovZoom(40.0f, 1, 0.1f), 40.0f * std::pow(0.45f, 0.1f))
              && Near(FovZoom(40.0f, -1, 0.1f), 40.0f / std::pow(0.45f, 0.1f))
              && FovZoom(4.0f, 1, 10.0f) == kFreecamFovMin
              && FovZoom(200.0f, -1, 10.0f) == kFreecamFovMax,
              "fov zoom is exponential, frame-delta limited and clamped");
        {
            float p[3] = {0, 0, 0};
            FreecamMove(p, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 2.0f);
            Check(Near3(p, 0, 0, 2), "freecam forward is world +Z at zero yaw");
            FreecamMove(p, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f);
            Check(Near3(p, -1, 0, 2), "freecam right is -X at zero yaw");
            FreecamMove(p, 0.0f, -90.0f, 1.0f, 0.0f, 0.0f, 1.0f);
            Check(Near(p[1], 1.0f), "looking up makes forward climb");
        }
        {
            const float testCam[3] = {0, 0, 0};
            float spot[4]{};
            SpotFromCamera(testCam, 90.0f, 0.0f, spot);
            Check(Near(spot[0], 4.24f) && Near(spot[1], 0.0f)
                  && Near(spot[2], -0.586f) && Near(spot[3], 90.0f),
                  "F10 inverts the derived camera layout back into a spot line");
        }
    }

    {
        Tuning t{};
        Layout l{};
        for (int s = 1; s <= kScenes; ++s) {
            const char* line = s == 1
                ? "camera 1 -35.613 -7.829 1036.500 -1.80 0.60 40.00"
                : "camera 2 274.204 21.594 64.607 120.00 4.20 40.00";
            Check(ParseLayoutLine(line, l), "layout camera line parsed for scene");
        }
        const float front[3] = {0, 0, 0}, back[3] = {2, 0, 0};
        const float cam[3] = {0, 1, -4};
        LightSlot cine[3];
        CineSlots(3, front, cam, 0.25f, cine);
        Check(cine[0].enabled && cine[1].enabled && cine[2].enabled
              && Near(cine[0].intensity, 0.70f * 0.25f)
              && Near(cine[1].intensity, 1.80f * 0.25f)
              && Near(cine[0].cone, 70.0f) && cine[0].shadows && cine[0].soft == 1.0f,
              "cinematic setup C fills all three slots around the front chest");
        CineSlots(0, front, cam, 1.0f, cine);
        Check(!cine[0].enabled, "setup zero leaves the layout's lights in charge");
        LightSlot lay[3];
        LayoutSlots(Layout{}, 1, cam, front, back, t.light, 0.13f, lay);
        Check(lay[0].enabled && !lay[2].enabled
              && Near(lay[0].intensity, 6.0f * 0.13f)
              && Near(lay[0].target[1], 1.25f),
              "layout lights scale with the stage strength and leave slot 2 dark");

        Check(CanSpawn(1000, 499, false, 5) && !CanSpawn(1000, 501, false, 5)
              && !CanSpawn(1000, 499, true, 5) && !CanSpawn(1000, 499, false, 4),
              "a preview spawn waits for the gap, the kill queue and the clear frames");
        Check(CanRetire(6000, 900, 3999, 5000, false, 11, true)
              && !CanRetire(6000, 900, 4001, 5000, false, 11, true)
              && !CanRetire(6000, 900, 3999, 5201, false, 11, true)
              && !CanRetire(6000, 900, 3999, 5000, true, 11, true)
              && !CanRetire(6000, 900, 3999, 5000, false, 10, true)
              && CanRetire(6000, 900, 3999, 5000, false, 1, false),
              "a waiting model is removed only when quiet, old and outside the pool");
    }

    if (fails) {
        std::printf("customize stage core: %d check(s) failed\n", fails);
        return 1;
    }
    std::printf("customize stage core: all checks passed\n");
    return 0;
}
