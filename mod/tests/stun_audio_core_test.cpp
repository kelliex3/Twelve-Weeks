// Standalone harness for the three pure cores behind the native stun audio owners
// (no game / engine headers):
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src -I mod/src/net/audio
//       mod/tests/stun_audio_core_test.cpp -o /tmp/stun_audio && /tmp/stun_audio

#include "stun-tone-core.hpp"
#include "crossbow-hud-core.hpp"
#include "stun-cough-core.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

    bool Near(float a, float b, float eps = 1e-3f)
    {
        return std::fabs(a - b) <= eps;
    }

    // ---- stun tone bed ----------------------------------------------------------

    void TestToneBed()
    {
        namespace core = net::audio::stun_tone::core;
        std::vector<core::Sample> l, r;
        // Packed stereo: channel 1 starts `frames` samples in, so provide two full
        // waves (the private ext blobs are at least this long).
        std::vector<core::Sample> impact(80, 10000), ring(80, 0);

        core::BuildBed(l, r, impact.data(), 40, ring.data(), 40, 10);
        assert(l.size() == 34 && r.size() == 34);
        // The ext's 0.12 s envelope shapes the RING only; the impact crack starts
        // at frame 0 unattenuated (x = rs*env + hit*hf).
        assert(l[0] == 10000);
        assert(l[1] == 10000);
        assert(l[10] == 10000);                    // steady body
        assert(l[32] == 5000 && l[33] == 2500);    // impact's own 0.4 s tail fade
        assert(l == r);

        std::vector<core::Sample> loud(80, 32767);  // int16 maximum: still clamped
        core::BuildBed(l, r, loud.data(), 40, ring.data(), 40, 10);
        assert(l[10] == 32000 && l[10] == r[10]);  // ext's +/-32000 clamp

        std::vector<core::Sample> quiet(80, 0), ringing(80, 10000);
        core::BuildBed(l, r, quiet.data(), 40, ringing.data(), 40, 10);
        assert(l[2] == 0);                         // ring starts at 0.30 s (frame 3)
        assert(l[3] == 0);                         // smoothstep at ri == 0 is zero
        assert(Near(static_cast<float>(l[4]), 1562.0f, 2.0f)); // rin = 0.25
        assert(l[7] == 10000);                     // fully faded in at 0.40 s
    }

    // ---- stun tone state machine -------------------------------------------------

    void TestToneBurstAndFade()
    {
        namespace core = net::audio::stun_tone::core;
        const float local[3] = { 0.0f, 0.0f, 0.0f };
        const float inRange[3] = { 3.0f, 1.0f, 0.0f };  // dy = 1 - (0 + 1) == 0
        const float tooFar[3] = { 3.01f, 1.0f, 0.0f };
        assert(Near(core::BurstDistance(inRange, local), 3.0f));
        assert(core::BurstInRange(inRange, local, 2.5f));       // radius + 0.5
        assert(!core::BurstInRange(tooFar, local, 2.5f));

        core::State s;
        assert(!core::VoiceUsable(s, 1000, true));
        core::Start(s, 1000, false);
        assert(!s.confirmed && s.started);
        assert(core::VoiceUsable(s, 2499, true));              // < 1500 ms
        assert(!core::VoiceUsable(s, 2500, true));
        assert(!core::VoiceUsable(s, 1200, false));            // a dead voice never dedupes

        // Unconfirmed at exactly 700 ms still plays; past 700 ms fades 150 ms.
        core::State unconfirmed;
        core::Start(unconfirmed, 1000, false);
        assert(core::Tick(unconfirmed, 1700, true, true, false) == core::Fade::kNone);
        assert(core::Tick(unconfirmed, 1701, true, true, false) == core::Fade::kNotStunned);
        assert(!unconfirmed.started);

        // Confirmation reuses the voice without replaying it.
        core::State confirmed;
        core::Start(confirmed, 1000, false);
        assert(core::CanConfirm(confirmed, 1200, true));
        core::Confirm(confirmed);
        assert(confirmed.confirmed && confirmed.t0 == 1000);
        assert(core::Tick(confirmed, 1900, true, true, true) == core::Fade::kNone);
        assert(core::Tick(confirmed, 1901, true, true, false) == core::Fade::kStunOver);

        core::State leftMatch;
        core::Start(leftMatch, 1000, true);
        assert(core::Tick(leftMatch, 1100, true, false, true) == core::Fade::kNotLive);

        core::State dead;
        core::Start(dead, 1000, true);
        assert(core::Tick(dead, 1100, false, true, true) == core::Fade::kInvalidate);

        core::Reset(dead);
        assert(!dead.started && !dead.confirmed && dead.t0 == 0);
    }

    // ---- crossbow HUD ------------------------------------------------------------

    void TestCrossbowHud()
    {
        namespace core = net::audio::crossbow_hud::core;
        core::State s;
        core::NoteBolt(s, 2, 100);

        core::Actions a = core::Tick(s, 1000, true, 1000);
        assert(a.impact && !a.pulse && !a.fadePulse && !a.pull);
        assert(s.hitSeen == 100 && s.nextPulse == 1900);

        a = core::Tick(s, 1899, true, 1000);
        assert(!a.impact && !a.pulse);
        a = core::Tick(s, 1900, true, 1000);
        assert(a.pulse && s.nextPulse == 2900);
        a = core::Tick(s, 2899, true, 1000);
        assert(!a.pulse);
        a = core::Tick(s, 2900, true, 1000);
        assert(a.pulse && s.nextPulse == 3900);

        // Same hit id does not retrigger; a new id does, and re-arms the pulse.
        core::NoteBolt(s, 2, 100);
        a = core::Tick(s, 4000, true, 1000);
        assert(!a.impact);
        core::NoteBolt(s, 2, 101);
        a = core::Tick(s, 4001, true, 1000);
        assert(a.impact && s.hitSeen == 101 && s.nextPulse == 4901);

        // Pull-out sound only on the transition off, and only when flagged.
        core::NoteClear(s, true);
        a = core::Tick(s, 5000, true, 1000);
        assert(!a.impact && !a.pulse && a.fadePulse && a.pull);
        a = core::Tick(s, 5001, true, 1000);
        assert(!a.pull && !a.fadePulse);
        core::NoteBolt(s, 3, 200);
        a = core::Tick(s, 6000, true, 1000);
        assert(a.impact);
        core::NoteClear(s, false);
        a = core::Tick(s, 6001, true, 1000);
        assert(!a.pull && a.fadePulse);

        // A bolt recorded while the match is not live never enters the state.
        core::Reset(s);
        core::NoteBolt(s, 1, 10);
        a = core::Tick(s, 7000, false, 1000);
        assert(!a.impact && !a.pulse && !a.fadePulse && !s.was);
        assert(core::PulsePeriodMs(2400, 48000) == 50);
        assert(core::PulsePeriodMs(0, 0) == 0);
    }

    // ---- stun cough --------------------------------------------------------------

    void TestCoughRules()
    {
        namespace core = net::audio::stun_cough::core;
        const core::WaveDesc waves[4] = {
            { 0, 10, false }, { 10, 10, false }, { 20, 10, true }, { 30, 10, true }
        };

        // Muffled bank: two poles per wave, but reset for each wave (no smear).
        std::vector<core::Sample> blob(40, 0), out, alone;
        for (int i = 0; i < 10; ++i)
            blob[i] = 10000;
        for (int i = 20; i < 30; ++i)
            blob[i] = -10000;
        core::BuildMuffled(out, blob.data(), waves, 4);
        assert(out.size() == 40);
        assert(out[0] < 10000 && out[9] > 0);       // low-pass ramp into a clamped tail
        assert(out[20] < 0 && out[20] > -10000);
        const core::WaveDesc femaleOnly[1] = { { 20, 10, true } };
        core::BuildMuffled(alone, blob.data(), femaleOnly, 1);
        for (int i = 20; i < 30; ++i)
            assert(out[i] == alone[i]);             // male wave does not leak in

        // Gender-filtered picks never repeat the previous wave for that gender.
        core::State picks;
        uint64_t now = 1000;
        int prevMale = -1, prevFemale = -1;
        for (int i = 0; i < 24; ++i) {
            const int m = core::PickWave(picks, now += 100, waves, 4, false);
            const int f = core::PickWave(picks, now += 100, waves, 4, true);
            assert(m == 0 || m == 1);
            assert(f == 2 || f == 3);
            assert(m != prevMale && f != prevFemale);
            prevMale = m;
            prevFemale = f;
        }
        assert(core::PickWave(picks, now, nullptr, 0, false) == -1);

        // Own cadence: first frame, then 1150 ms later -- never at 1149 ms.
        core::State s;
        core::NoteOwnStun(s, 1000);
        core::SetOwnFemale(s, true);
        core::Result r = core::Tick(s, 1000, true, true, nullptr, 0, waves, 4);
        assert(r.own && (r.ownWave == 2 || r.ownWave == 3));
        assert(!core::Tick(s, 2149, true, true, nullptr, 0, waves, 4).own);
        r = core::Tick(s, 2150, true, true, nullptr, 0, waves, 4);
        assert(r.own);
        assert(!core::Tick(s, 1000, false, true, nullptr, 0, waves, 4).own);

        // Peer freshness, cadence and the 3.2 s elapsed cap.
        core::State peer;
        bool ok[core::kCoreMaxPeers] = { true, true, true, true, true, true, true, true };
        const float pos[3] = { 1.0f, 2.0f, 3.0f };
        core::NotePeerStun(peer, 0, 1000, false, 7, pos);
        r = core::Tick(peer, 1000, true, true, ok, 8, waves, 4);
        assert(r.peerCount == 1 && r.peers[0].slot == 0 && r.peers[0].fresh);
        assert(r.peers[0].wave == 0 || r.peers[0].wave == 1);
        assert(r.peers[0].handle == 7 && r.peers[0].hasPos);

        r = core::Tick(peer, 1500, true, true, ok, 8, waves, 4);
        assert(r.peerCount == 0);                    // same stamp, inside cooldown
        core::NotePeerStun(peer, 0, 1001, false, 7, pos);
        r = core::Tick(peer, 1501, true, true, ok, 8, waves, 4);
        assert(r.peerCount == 1 && r.peers[0].fresh);

        r = core::Tick(peer, 2000, true, true, ok, 8, waves, 4);
        assert(r.peerCount == 0);                    // inside the 1150 ms cadence

        core::State oldStamp;
        core::NotePeerStun(oldStamp, 0, 1000, false, 7, pos);
        r = core::Tick(oldStamp, 4199, true, true, ok, 8, waves, 4);
        assert(r.peerCount == 1);                    // 3199 ms: just inside the cap
        core::State expired;
        core::NotePeerStun(expired, 0, 1000, false, 7, pos);
        r = core::Tick(expired, 4200, true, true, ok, 8, waves, 4);
        assert(r.peerCount == 0);                    // now - at == 3200: too old

        core::NotePeerStun(peer, 0, 5000, false, 0, pos);
        r = core::Tick(peer, 5000, true, true, ok, 8, waves, 4);
        assert(r.peerCount == 0);                    // no puppet handle, no voice
        core::ClearPeer(peer, 0);
        r = core::Tick(peer, 5001, true, true, ok, 8, waves, 4);
        assert(r.peerCount == 0);
    }

} // namespace

int main()
{
    TestToneBed();
    TestToneBurstAndFade();
    TestCrossbowHud();
    TestCoughRules();
    std::printf("STUN_AUDIO_CORE_TEST_OK\n");
    return 0;
}
