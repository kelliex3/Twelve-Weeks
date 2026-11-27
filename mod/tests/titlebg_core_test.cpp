#include "titlebg-core.hpp"

#include <cmath>
#include <cstring>
#include <iostream>

using namespace menu::titlebg::core;
namespace core = menu::titlebg::core;

namespace {

    int failed = 0;

    void Check(bool ok, const char* what)
    {
        if (!ok) ++failed;
        std::cout << (ok ? "ok   " : "FAIL ") << what << '\n';
    }

    bool Near(float a, float b, float eps = 0.001f)
    {
        return std::fabs(a - b) <= eps;
    }

} // namespace

int main()
{
    std::cout << "titlebg core:\n";

    {
        float v[6]{};
        Check(!ParsePose("14.855 2.957 530.968 150.19", v),
              "a pose needs position, yaw and pitch");
        Check(ParsePose("1 2 3 90 -4", v) && Near(v[3], 90.0f)
              && Near(v[5], 0.0f),
              "a five-value pose is accepted with fov 0");
        Check(ParsePose("1 2 3 90 -4 46.94", v) && Near(v[5], 46.94f),
              "the optional fov is read");
        Check(Near(Fov(v), 46.94f), "a positive fov is used");
        v[5] = 0.0f;
        Check(Near(Fov(v), kPose[5]), "fov 0 falls back to the shipped pose");
        Check(!ParsePose(nullptr, v) && !ParsePose("", v),
              "empty sidecar lines are ignored");
    }

    {
        Check(Near(Ease(-1.0f), 0.0f) && Near(Ease(2.0f), 1.0f),
              "the sway ramp clamps at both ends");
        Check(Near(Ease(0.5f), 0.5f), "the ramp is symmetric");
        const Pose first = FramePose(1000, 1000, nullptr);
        Check(Near(first.p[0], kPose[0]) && Near(first.p[1], kPose[1])
              && Near(first.p[2], kPose[2]) && Near(first.yaw, kPose[3])
              && Near(first.pitch, kPose[4]) && Near(first.fov, kPose[5]),
              "the first frame is exactly the default pose");

        const Pose sway = FramePose(1000 + 2000, 1000, nullptr);
        Check(std::fabs(sway.yaw - kPose[3]) <= kYawDeg + 0.0001f
              && std::fabs(sway.pitch - kPose[4]) <= kPitchDeg + 0.0001f,
              "the sway stays within its angular budget");
        for (int k = 0; k < 3; ++k)
            Check(std::fabs(sway.p[k] - kPose[k]) <= kPosM + 0.0001f,
                  "the sway stays within its 4 cm budget");
        Check(Near(FramePose(100000, 1000, nullptr).fov, kPose[5]),
              "the fov never drifts");
    }

    {
        float q[4]{};
        Quat(0.0f, 0.0f, q);
        Check(Near(q[0], 0.0f) && Near(q[1], 0.0f)
              && Near(q[2], 0.0f) && Near(q[3], 1.0f),
              "a zero rotation is the identity quaternion");
        Quat(90.0f, 0.0f, q);
        Check(Near(q[1], std::sqrt(0.5f)) && Near(q[3], std::sqrt(0.5f)),
              "yaw rotates around +Y");
        Quat(0.0f, -3.23f, q);
        Check(q[0] < 0.0f && q[3] > 0.0f,
              "a negative pitch looks slightly up");
    }

    {
        Layer l;
        Check(!ParseLayer("# a comment", l), "comments are ignored");
        Check(!ParseLayer("*res-rs-005-towerhill-vista* 100", l),
              "a layer needs priority and strength");
        Check(ParseLayer("*rs-x* 120 0.75", l)
              && !std::strcmp(l.name, "*rs-x*")
              && l.prio == 120 && Near(l.str, 0.75f),
              "a layer sidecar line parses");
        DefaultLayer(l);
        Check(!std::strcmp(l.name, kDefaultLayer) && l.prio == kRsPrio
              && Near(l.str, 1.0f),
              "the missing sidecar falls back to the overlook layer");
    }

    {
        Check(!BootCanStart(5, 0) && BootCanStart(6, 0)
              && !BootCanStart(100, 1),
              "the black cover starts after frame 5 in substate 0");
        Boot b;
        BootBegin(b, 5000, 3);
        Check(b.state == 1 && b.t0 == 5000 && b.hits0 == 3,
              "the cover records its start and camera-write baseline");
        Check(!BootTick(b, 5500, 8, false, false),
              "the cover waits for the title page");
        for (int i = 0; i < core::kBootRs; ++i)
            Check(!BootTick(b, 5600 + i, 3 + core::kBootHits, true, true),
                  "render-handle readiness accumulates");
        Check(b.rsOk == core::kBootRs
              && b.readyAt == 5600 + core::kBootRs - 1,
              "camera hits plus render handles mark the boot ready");
        Check(!BootTick(b, 5700, 3 + core::kBootHits, true, false)
              && b.readyAt == 0,
              "losing the title page drops readiness");
        Check(!BootTick(b, 7000, 3 + core::kBootHits, true, true),
              "the settle delay still holds the cover");
        Check(BootTick(b, 7000 + kBootSettleMs,
                       3 + core::kBootHits, true, true),
              "the cover lifts after the settle delay");
        Check(b.state == 2 && b.fadeAt != 0, "the fade-up is one-shot");
        Check(Near(ButtonAlpha(b.fadeAt, b), 0.0f)
              && Near(ButtonAlpha(b.fadeAt + 600, b), 0.5f)
              && Near(ButtonAlpha(b.fadeAt + 1200, b), 1.0f),
              "the title buttons ease in with the world");

        Boot timeout;
        BootBegin(timeout, 0, 0);
        Check(!BootTick(timeout, 10, 10, true, true),
              "a title page starts the timeout clock");
        Check(BootTick(timeout, kBootMaxMs + 10, 10, true, true),
              "the safety timeout lifts the cover without readiness");
    }

    if (failed) {
        std::cout << failed << " titlebg core check(s) failed\n";
        return 1;
    }
    std::cout << "titlebg core: all checks passed\n";
    return 0;
}
