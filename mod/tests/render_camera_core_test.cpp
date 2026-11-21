// Standalone harness for the pure final-render-camera helpers (no game headers):
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/engine
//       mod/tests/render_camera_core_test.cpp -o /tmp/render_camera_core_test
//       && /tmp/render_camera_core_test

#include "render-camera.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace {

    bool Near(float a, float b, float eps = 1e-4f)
    {
        return std::fabs(a - b) <= eps;
    }

    void TestForwardFromQuaternion()
    {
        using engine::render_camera::ForwardFromQuaternion;

        float f[3] = {};

        // Identity: the camera's view axis is +Z.
        const float id[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        ForwardFromQuaternion(id, f);
        assert(Near(f[0], 0.0f) && Near(f[1], 0.0f) && Near(f[2], 1.0f));

        // +90 deg about Y sends +Z to +X.
        const float y90[4] = { 0.0f, 0.70710678f, 0.0f, 0.70710678f };
        ForwardFromQuaternion(y90, f);
        assert(Near(f[0], 1.0f) && Near(f[1], 0.0f) && Near(f[2], 0.0f));

        // -90 deg about X sends +Z to +Y (R_x(-90) * Z = +Y).
        const float xm90[4] = { -0.70710678f, 0.0f, 0.0f, 0.70710678f };
        ForwardFromQuaternion(xm90, f);
        assert(Near(f[0], 0.0f) && Near(f[1], 1.0f) && Near(f[2], 0.0f));

        // Result is unit length for a unit quaternion (the real code gates on QuaternionOk).
        const float raw[4] = { 0.3f, -0.5f, 0.2f, 0.8f };
        const float n = std::sqrt(raw[0] * raw[0] + raw[1] * raw[1]
                                + raw[2] * raw[2] + raw[3] * raw[3]);
        const float q[4] = { raw[0] / n, raw[1] / n, raw[2] / n, raw[3] / n };
        ForwardFromQuaternion(q, f);
        const float len2 = f[0] * f[0] + f[1] * f[1] + f[2] * f[2];
        assert(Near(len2, 1.0f, 2e-4f));
    }

    void TestQuaternionOk()
    {
        using engine::render_camera::QuaternionOk;

        const float ok[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        assert(QuaternionOk(ok));

        const float tilted[4] = { 0.5f, 0.5f, 0.5f, 0.5f }; // unit length
        assert(QuaternionOk(tilted));

        const float tooLong[4] = { 0.0f, 0.0f, 0.0f, 2.0f };
        assert(!QuaternionOk(tooLong));

        const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        assert(!QuaternionOk(zero));

        const float nan[4] = { std::nanf(""), 0.0f, 0.0f, 1.0f };
        assert(!QuaternionOk(nan));
    }

    void TestFresh()
    {
        using engine::render_camera::Fresh;
        using engine::render_camera::kMaxAgeMs;

        assert(!Fresh(1000, 0));                 // never captured
        assert(Fresh(1000, 900));                // 100 ms old
        assert(Fresh(1000, 1000 - kMaxAgeMs));   // exactly at the age limit
        assert(!Fresh(1000, 1000 - kMaxAgeMs - 1));
        assert(!Fresh(800, 900));                // stamp in the future
    }

} // namespace

int main()
{
    TestForwardFromQuaternion();
    TestQuaternionOk();
    TestFresh();
    std::printf("RENDER_CAMERA_CORE_TEST_OK\n");
    return 0;
}
