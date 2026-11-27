#include "sendoff-core.hpp"

#include <cmath>
#include <iostream>

using namespace menu::sendoff::core;

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

    float Dot(const float a[3], const float b[3])
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

} // namespace

int main()
{
    std::cout << "sendoff core:\n";

    {
        Check(Near(Duck(0), 1.0f), "the session starts at full volume");
        Check(Near(Duck(kDipMs), std::pow(10.0f, kDipDb / 20.0f)),
              "the music eases down to the -10 dB dip in 600 ms");
        Check(Near(Duck(kDipMs + 100), std::pow(10.0f, kDipDb / 20.0f)),
              "the dip holds after 600 ms");
        Check(Near(BlackFade(0), 1.0f) && Near(BlackFade(kBlackMs), 1.0f),
              "the picture is full until the black fade");
        Check(Near(BlackFade(kBlackMs + 1200), 0.5f),
              "the black fade is linear-to-zero");
        Check(Near(BlackFade(kEndMs), 0.0f),
              "the picture is silent at the level-start point");
        Check(Near(Compensate(1.0f, 0.25f), kCompCap),
              "compensation is capped at 4x");
        Check(Near(Compensate(1.0f, 0.5f), 2.0f),
              "compensation divides out the session dip");
    }

    {
        Check(Near(Environment(kFacMs), 1.0f)
              && Near(Environment(kFullMs), 1.0f),
              "the faction clip is full into the black");
        Check(Near(Environment((kFullMs + kFadeMs) / 2), 0.25f),
              "the clip fades down quadratically");
        Check(Near(Environment(kFadeMs), 0.0f), "the clip ends by 7600 ms");
        Check(kRoute[0] == kNone && kRoute[1] == kExt && kRoute[2] == kExt,
              "both faction scenes use our own crow flock by default");
        Check(Near(Far(kWlf), 24.0f) && Near(Far(kSer), 14.0f),
              "the flock distance is scene-specific");
    }

    {
        const CamBasis wlf = Camera(kWlf), ser = Camera(kSer);
        const float f0[3] = { 0.0f, 0.0f, 1.0f };
        Check(Near(std::sqrt(Dot(wlf.f, wlf.f)), 1.0f)
              && Near(std::sqrt(Dot(ser.f, ser.f)), 1.0f),
              "the menu forward vectors are normalized");
        Check(Near(Dot(wlf.f, wlf.r), 0.0f, 0.0001f)
              && Near(Dot(ser.f, ser.r), 0.0f, 0.0001f),
              "right is perpendicular to forward");
        Check(wlf.u[1] > 0.0f && ser.u[1] > 0.0f,
              "up points up on both menu screens");
        Check(Near(wlf.th, std::tan(kWlfFov * 0.5f * kRad))
              && Near(ser.th, std::tan(kSerFov * 0.5f * kRad)),
              "the half-angle tangent comes from the scene FOV");

        float q[4]{};
        QuatDir(f0, q);
        Check(Near(q[0], 0.0f) && Near(q[1], 0.0f)
              && Near(q[2], 0.0f) && Near(q[3], 1.0f),
              "a +Z velocity is the identity view rotation");
        const float right[3] = { 1.0f, 0.0f, 0.0f };
        QuatDir(right, q);
        Check(Near(q[1], std::sqrt(0.5f)) && Near(q[3], std::sqrt(0.5f)),
              "a +X velocity is a 90 degree yaw");
    }

    {
        uint32_t a = 0x9e3779b9u, b = 0x9e3779b9u;
        const uint32_t x = NextRand(a), y = NextRand(b);
        Check(x == y && NextRand(a) == NextRand(b),
              "the flock RNG is deterministic");
        bool inRange = true;
        for (int i = 0; i < 32; ++i) {
            const float r = Rand01(b);
            inRange = inRange && r >= 0.0f && r < 1.0f;
        }
        Check(inRange, "the RNG stays in [0,1)");

        const CamBasis cam = Camera(kWlf);
        Bird bird;
        uint32_t rng = 0x12345678u;
        PlaceBird(bird, cam, 0, 1, 24.0f, cam.th, rng);
        float rel[3];
        for (int k = 0; k < 3; ++k) rel[k] = bird.p[k] - cam.c[k];
        Check(Dot(rel, cam.u) < 0.0f,
              "a crow starts below the bottom of the frame");
        Check(Dot(rel, cam.r) < 0.0f,
              "a left-side crow starts on the left of the frame");
        Check(Dot(bird.v, cam.u) > 0.0f,
              "a crow climbs away from the camera");
        Check(bird.delay >= 0 && bird.delay < 450,
              "takeoffs are staggered within 450 ms");

        const float before[3] = { bird.p[0], bird.p[1], bird.p[2] };
        float p[3], q[4];
        Check(StepBird(bird, cam, 0.016f, 1.0f, 0, p, q),
              "a live crow survives a flight step");
        Check(p[0] != before[0] || p[1] != before[1] || p[2] != before[2],
              "the engine locator is given a new position");
        const float qn = std::sqrt(q[0] * q[0] + q[1] * q[1]
                                   + q[2] * q[2] + q[3] * q[3]);
        Check(Near(qn, 1.0f),
              "the crow's facing quaternion is a unit quaternion");

        Bird anim;
        anim.delay = 0;
        anim.phase = 0;
        Check(!FlyAnimDue(anim, 1000, 0.6f, 600),
              "the fly cycle does not start before 700 ms");
        Check(FlyAnimDue(anim, 1000, 0.8f, 600),
              "takeoff hands over to the fly cycle");
        anim.phase = 1;
        anim.animAt = 1000;
        anim.animMs = 600;
        Check(!FlyAnimDue(anim, 1500, 1.0f, 600)
              && FlyAnimDue(anim, 1600, 1.0f, 600),
              "the fly cycle reissues when its duration ends");
    }

    if (failed) {
        std::cout << failed << " sendoff core check(s) failed\n";
        return 1;
    }
    std::cout << "sendoff core: all checks passed\n";
    return 0;
}
