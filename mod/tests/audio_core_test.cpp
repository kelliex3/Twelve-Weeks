// Standalone harness for the pure shiv / exec / music-hold / stun / xcam cores (no game headers):
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/gameplay -I mod/src/net/audio
//       mod/tests/audio_core_test.cpp mod/src/gameplay/stun-lock.cpp
//       mod/src/net/audio/music-hold.cpp mod/src/net/audio/exec-music.cpp
//       mod/src/net/audio/xcam.cpp -o /tmp/audio_core_test && /tmp/audio_core_test
// (the music-tier value helpers are inline in its header; no game header is pulled in)

#include "stun-lock.hpp"
#include "music-hold.hpp"
#include "exec-music.hpp"
#include "xcam.hpp"
#include "music-tier.hpp"
#include "music-sight.hpp"

#include <cassert>
#include <cstdio>

namespace {

    struct LogSink final : gameplay::stun_lock::Sink {
        int lines = 0;
        void Log(const char*) override { ++lines; }
    };

    struct HoldSink final : net::audio::music_hold::Sink {
        uint64_t played = 0;
        uint64_t current = 0;
        uint64_t PlayMusic(uint64_t sid) override { played = sid; return 0; }
        uint64_t CurrentMusic() override { return current; }
        void Log(const char*) override {}
    };

    struct ExecSink final : net::audio::exec_music::Sink {
        int cuts = 0, starts = 0, cueCuts = 0;
        int lastPick = -1, lastFade = 0;
        double lastSeek = 0.0;
        bool live = false;
        void MusicCut() override { ++cuts; }
        void StartCue(int pick, double seek, float) override
        { ++starts; lastPick = pick; lastSeek = seek; live = true; }
        bool CueLive() const override { return live; }
        void CutCue(int fadeMs) override { ++cueCuts; lastFade = fadeMs; }
        void Log(const char*) override {}
    };

    struct XcamSink final : net::audio::xcam::Sink {
        int exec = 0, shiv = 0, upgraded = 0;
        float kill = 0.0f;
        void Exec(float killS) override { ++exec; kill = killS; }
        void Shiv(const char*) override { ++shiv; }
        void Upgraded(const char*) override { ++upgraded; }
        void Log(const char*) override {}
    };

    void TestStun()
    {
        using namespace gameplay::stun_lock;
        LogSink log;
        SetSink(&log);
        Reset();
        assert(TotalMs() == 3500 && BlendOutMs() == 350 && CoughMs() == 1200);
        assert(!Active(1000) && !RecentlyEnded(1000));
        Stun(1000, 3);
        assert(Fresh() && FromSlot() == 3 && Until() == 4500);
        assert(Active(4499) && !Active(4500));
        assert(RecentlyEnded(5600));            // 1.1 s into the grace
        assert(!RecentlyEnded(5700));           // 1.2 s: grace over
        assert(VictimOk(true, 0.0f, 1.0f, 2000));
        assert(!VictimOk(true, 0.0f, 1.0f, 5700));
        assert(VictimOk(false, 180.0f, 3.0f, 5700));   // back arc, no stun needed
        assert(!VictimOk(false, 90.0f, 3.0f, 5700));
        Reset();
        Stun(1000, 4);
        Stun(2000, 5);                          // re-stun restarts the timer
        assert(!Fresh() && Until() == 5500);
        Cut(2100);
        assert(!Active(2100) && RecentlyEnded(2100) && EndedAt() == 2100);
        Reset();
        Stun(1000, 6);
        Tick(1200, false);                      // match left: phase end, grace stamped
        assert(!Active(1200) && RecentlyEnded(1200));
        assert(log.lines > 0);
        SetSink(nullptr);
        Reset();
    }

    void TestMusicHold()
    {
        using namespace net::audio::music_hold;
        HoldSink sink;
        SetSink(&sink);
        Reset();
        assert(Fnv64("") == 0xCBF29CE484222325ULL);
        assert(MusicSid("x") == Fnv64("*x*"));
        const uint64_t sid = Play("music-playlist-rogue-player-death", "test");
        assert(sid == Fnv64("*music-playlist-rogue-player-death*"));
        assert(sink.played == sid);
        sink.current = static_cast<uint64_t>(-1);
        assert(Current() == kNoPlaylist);
        sink.current = sid;
        assert(Current() == sid);
        Hold(5000);
        assert(HoldUntil() == 5000 && ResumeAt() == 5000 && BlocksDown(4999) && !BlocksDown(5000));
        assert(!ResumeDue(4999) && ResumeDue(5000) && TakeResume(5000) == 5000);
        assert(ResumeAt() == 0 && TakeResume(5000) == 0);
        NowatchUntil(7000);
        assert(BlocksWatch(6999) && !BlocksWatch(7000));
        SetDead(true);
        assert(Dead() && BlocksWatch(9999));
        SetDead(false);
        Reset();
        assert(HoldUntil() == 0 && ResumeAt() == 0 && NowatchUntil() == 0 && !Dead());
        SetSink(nullptr);
    }

    void TestExecMusic()
    {
        using namespace net::audio::exec_music;
        net::audio::music_hold::Reset();
        Reset();
        ExecSink sink;
        SetSink(&sink);

        Start(1000.0, 2.5f, 0);   // hit 2.44 lands on the death screen at 2.653 s
        assert(PendingAt() > 1000 && PendingAt() < 1400);
        assert(net::audio::music_hold::HoldUntil() == PendingAt() + kCapMs + kResumeMs);
        assert(!Pending(1050));
        const uint64_t at = PendingAt();
        Tick(at, false);               // not in a running match: no cue starts
        assert(!Playing() && sink.starts == 0);
        Tick(at, true);
        assert(Playing() && sink.starts == 1 && sink.lastPick == 0 && sink.cuts == 1);
        assert(sink.lastSeek == 0.0);
        assert(net::audio::music_hold::HoldUntil() == at + kCapMs + kResumeMs);

        Reset();
        net::audio::music_hold::Reset();
        sink = ExecSink{};
        Start(1000.0, 2.0f, 0);    // hit is late: join mid-cue
        assert(PendingAt() == 1000);
        Tick(1000, true);
        assert(Playing() && sink.lastSeek > 0.28 && sink.lastSeek < 0.29);
        Tick(1000 + kCapMs - 1, true);
        assert(!Fading());
        Tick(1000 + kCapMs, true);
        assert(Fading() && sink.cueCuts == 1 && sink.lastFade == 300);

        Reset();
        net::audio::music_hold::Reset();
        sink = ExecSink{};
        Start(1000.0, 2.5f, 0);
        Fade(1100, "aborted");     // a pending cue never starts after a fade
        assert(PendingAt() == 0);
        Tick(5000, true);
        assert(sink.starts == 0);
        SetSink(nullptr);
        Reset();
        net::audio::music_hold::Reset();
    }

    void TestXcam()
    {
        using namespace net::audio::xcam;
        XcamSink sink;
        SetSink(&sink);
        assert(!Route(static_cast<int>(Role::kAttacker), "shiv-stealth-back", false).shiv);
        auto r = Route(static_cast<int>(Role::kVictim), "ground-execution", false);
        assert(r.execMusic && !r.shiv);
        r = Route(static_cast<int>(Role::kVictim), "shiv-ground-execution-01", false);
        assert(r.execMusic && !r.shiv);
        r = Route(static_cast<int>(Role::kVictim), "shiv-stealth-back", false);
        assert(r.shiv && !r.execMusic);
        r = Route(static_cast<int>(Role::kVictim), "ctxdown-mqte", false);
        assert(!r.execMusic && !r.shiv);
        r = Route(static_cast<int>(Role::kVictim), "pipe-mod-takedown", false);
        assert(r.upgraded && !r.execMusic);
        r = Route(static_cast<int>(Role::kVictim), "shiv-stealth-back", true);
        assert(r.shiv && !r.execMusic);   // a shiv still wins over the upgraded flag

        Dispatch(static_cast<int>(Role::kVictim), "shiv-stealth-back", 0.36f, false);
        Dispatch(static_cast<int>(Role::kVictim), "rifle-execution", 2.4f, false);
        Dispatch(static_cast<int>(Role::kVictim), "pipe-mod-takedown", 0.5f, false);
        assert(sink.shiv == 1 && sink.exec == 1 && sink.upgraded == 1);
        assert(sink.kill == 2.4f);
        SetSink(nullptr);
        Reset();
    }

    void TestTierHelpers()
    {
        using namespace net::audio::music_tier;
        assert(ShotHeat(true, 10.0f) == 2.0f && ShotHeat(true, 60.0f) == 1.2f);
        assert(ShotHeat(false, 10.0f) == 1.0f && ShotHeat(false, 60.0f) == 0.6f);
        assert(NearFireHeat(false) == 1.5f && NearFireHeat(true) == 0.75f);
        assert(CloseHeat() == 3.0f && DownHeat() == 4.0f && ReviveHeat() == 4.0f && StunHeat() == 5.0f);
        assert(WeStunnedHeat() == 6.0f);
        assert(IsAwareCause("enemy stunned") && IsAwareCause("we took a hit"));
        assert(IsAwareCause("spotted by an enemy (spot call)") && IsAwareCause("we hit an enemy"));
        assert(!IsAwareCause("we fired") && !IsAwareCause("health dropped"));
        // ext MusicSlotSeen frustum: in front + forward cone + minimum range.
        assert(net::audio::music_sight::SeenFrustumOk(10.0f, 10.0f));       // straight ahead
        assert(!net::audio::music_sight::SeenFrustumOk(0.0f, 10.0f));       // behind
        assert(!net::audio::music_sight::SeenFrustumOk(1.0f, 10.0f));       // > 53 deg off-axis
        assert(!net::audio::music_sight::SeenFrustumOk(0.05f, 0.1f));       // < 0.5 m depth
        assert(!net::audio::music_sight::SeenFrustumOk(0.05f, 0.05f));      // < 0.1 m range
        assert(net::audio::xcam::Contains("shiv-ground-execution-2", "ground-execution"));
        assert(!net::audio::xcam::Contains(nullptr, "shiv"));
    }

}

int main()
{
    TestStun();
    TestMusicHold();
    TestExecMusic();
    TestXcam();
    TestTierHelpers();
    std::puts("AUDIO_CORE_TEST_OK");
    return 0;
}
