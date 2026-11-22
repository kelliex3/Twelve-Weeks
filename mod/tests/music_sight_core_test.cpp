// Standalone harness for the pure ext MusicSlotSeen / body-visibility helpers (no game headers):
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/net/audio
//       mod/tests/music_sight_core_test.cpp -o /tmp/music_sight_core_test
//       && /tmp/music_sight_core_test

#include "music-sight.hpp"

#include <cassert>
#include <cstdio>

namespace {

    using net::audio::music_sight::BodyVisClear;
    using net::audio::music_sight::BodyVisFraction;
    using net::audio::music_sight::SeenFrustumOk;
    using net::audio::music_sight::TagOnScreen;

    void TestSeenFrustum()
    {
        // ext MusicSlotSeen: depth >= 0.5, len >= 0.1, depth/len >= 0.6.
        assert(!SeenFrustumOk(0.49f, 1.0f));        // too close in depth
        assert(!SeenFrustumOk(0.6f, 0.09f));        // too close in range
        assert(!SeenFrustumOk(0.6f, 1.1f));         // depth/len 0.545 < 0.6
        assert(SeenFrustumOk(0.5f, 0.83f));         // depth/len ~0.602 >= 0.6
        assert(SeenFrustumOk(8.0f, 10.0f));         // 0.8, in front and in cone
        assert(!SeenFrustumOk(-1.0f, 1.0f));        // behind the camera
    }

    void TestBodyVisClear()
    {
        // ext: (t >= 0 && t < 1 && (1 - t) * dist > 0.35 m) == unoccluded.
        assert(BodyVisClear(0.0f, 10.0f));          // nothing in front of the point
        assert(BodyVisClear(0.5f, 10.0f));          // wall leaves 5 m of clearance
        assert(!BodyVisClear(0.5f, 0.5f));          // only 0.25 m left -> own cover
        assert(BodyVisClear(0.96f, 10.0f));         // 0.4 m left -> clear
        assert(!BodyVisClear(0.97f, 10.0f));        // only 0.3 m left -> blocked
        assert(!BodyVisClear(-1.0f, 10.0f));        // RayT no-result
        assert(!BodyVisClear(-2.0f, 10.0f));        // RayT no job
        assert(!BodyVisClear(1.0f, 10.0f));         // hit at the point itself
        assert(!BodyVisClear(1.2f, 10.0f));         // hit past the point
    }

    void TestBodyVisFraction()
    {
        // ext: weighted clear fraction > 5% == visible.
        assert(BodyVisFraction(1.0f, 1.0f));
        assert(BodyVisFraction(0.06f, 1.0f));
        assert(!BodyVisFraction(0.05f, 1.0f));      // strictly greater than 5%
        assert(!BodyVisFraction(0.0f, 1.0f));
        assert(!BodyVisFraction(1.0f, 0.0f));       // no answers -> not visible
        // ext weights are integers (1 or 2); 1 clear of 12 units (sum 16) is 6.25%.
        assert(BodyVisFraction(1.0f, 16.0f));
        assert(!BodyVisFraction(0.5f, 16.0f));
    }

    void TestTagOnScreen()
    {
        // 16:9: the box is exactly 0..1280 x 0..720 (no text offset).
        assert(TagOnScreen(0.0f, 0.0f, 0.0f));
        assert(TagOnScreen(640.0f, 360.0f, 0.0f));
        assert(TagOnScreen(1280.0f, 720.0f, 0.0f));
        assert(!TagOnScreen(-0.01f, 360.0f, 0.0f));
        assert(!TagOnScreen(1280.01f, 360.0f, 0.0f));
        assert(!TagOnScreen(640.0f, -0.01f, 0.0f));
        assert(!TagOnScreen(640.0f, 720.01f, 0.0f));

        // Ultrawide: g_text_xoff widens the accepted x range symmetrically.
        assert(TagOnScreen(-100.0f, 360.0f, 100.0f));
        assert(TagOnScreen(1380.0f, 360.0f, 100.0f));
        assert(!TagOnScreen(-100.01f, 360.0f, 100.0f));
        assert(!TagOnScreen(1380.01f, 360.0f, 100.0f));
    }

} // namespace

int main()
{
    TestSeenFrustum();
    TestBodyVisClear();
    TestBodyVisFraction();
    TestTagOnScreen();
    std::puts("MUSIC_SIGHT_CORE_TEST_OK");
    return 0;
}
