// Pure-core checks for the ext xcam_start role-2 routing used by xcam-bridge.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src -I mod/src/net/audio
//        mod/tests/xcam_core_test.cpp mod/src/net/audio/xcam.cpp -o /tmp/xcam && /tmp/xcam

#include "xcam.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

    int failures = 0;

    void Check(bool ok, const char* what)
    {
        if (!ok) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    }

    void Expect(const char* name, bool exec, bool shiv, bool upg)
    {
        const net::audio::xcam::Result r =
            net::audio::xcam::Route(static_cast<int>(net::audio::xcam::Role::kVictim), name, false);
        if (r.execMusic != exec || r.shiv != shiv || r.upgraded != upg) {
            std::fprintf(stderr, "FAIL: route \"%s\": got exec=%d shiv=%d upg=%d, want %d/%d/%d\n",
                         name, r.execMusic, r.shiv, r.upgraded, exec, shiv, upg);
            ++failures;
        }
    }

}

int main()
{
    using net::audio::xcam::Route;

    // Only the victim's client dispatches.
    const auto attacker = Route(static_cast<int>(net::audio::xcam::Role::kAttacker),
                                "shiv-stealth-back", false);
    Check(!attacker.execMusic && !attacker.shiv && !attacker.upgraded, "attacker role is inert");
    const auto idle = Route(0, nullptr, false);
    Check(!idle.execMusic && !idle.shiv && !idle.upgraded, "idle role is inert");

    // The ext's three role-2 branches, with the real MELEE2013 names the native producer
    // reconstructs from gameplay/melee2013::At().
    Expect("fist-execute-enter-back", true, false, false);
    Expect("shiv-stealth-back", false, true, false);
    Expect("shiv-stealth-front", false, true, false);
    Expect("fist-insta-shiv-stand-front", false, true, false);
    Expect("shiv-interrogate-enter-back", false, true, false);
    Expect("shiv-ground-execution-left", true, false, false);   // ground shiv = normal exec music
    Expect("pipe-mod-chop-fin", false, false, true);            // upgraded one-shot = no music
    Expect("ctxdown:pu-drag", false, false, false);             // contextual down suppresses both
    Expect("melee-finisher", true, false, false);               // ext's fallback name

    // The ext flag is separate from the name: an upgraded victim never gets exec music.
    const auto upg = Route(static_cast<int>(net::audio::xcam::Role::kVictim),
                           "fist-execute-enter-back", true);
    Check(!upg.execMusic && !upg.shiv && upg.upgraded, "upgraded flag suppresses exec music");

    Check(net::audio::xcam::Contains("shiv-ground-execution-left", "shiv"), "contains shiv");
    Check(!net::audio::xcam::Contains(nullptr, "shiv"), "null haystack");
    Check(!net::audio::xcam::Contains("shiv", nullptr), "null needle");

    if (failures)
        return EXIT_FAILURE;
    std::puts("xcam_core_test: OK");
    return EXIT_SUCCESS;
}
