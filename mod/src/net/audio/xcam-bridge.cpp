// gated bridge: pure xcam routing -> exec music / shiv sting (both themselves sunk)

#include "xcam-bridge.hpp"

#include "exec-music.hpp"
#include "shivsting.hpp"
#include "xcam.hpp"

#include "log.hpp"

#include <windows.h>

#include <cstdint>
#include <random>

namespace net::audio::xcam_bridge {

    namespace {

        struct Bridge final : xcam::Sink {
            void Exec(float killS) override
            {
                static std::mt19937 rng{ std::random_device{}() };
                const int pick = static_cast<int>(rng() % static_cast<uint32_t>(exec_music::kCues));
                exec_music::Start(GetTickCount64(), killS, pick);
            }
            void Shiv(const char* name) override
            {
                shivsting::ShivMusicStart(name);   // ext: ShivdrumStart first
                shivsting::Start(name);            // then ShivstingStart
            }
            void Upgraded(const char* name) override
            {
                TO1_LOG("[XCamBridge] %s - upgraded melee one-shot, no execution music\n",
                        name ? name : "?");
            }
            void Log(const char* line) override
            {
                TO1_LOG("[XCamBridge] %s\n", line);
            }
        };

        Bridge g_bridge;

    }

    void Init()
    {
        if (kEnabled)
            xcam::SetSink(&g_bridge);
    }

    void OnCameraStart(int role, const char* name, float killS, bool upgradedOneShot)
    {
        if (!kEnabled)
            return;
        xcam::Dispatch(role, name, killS, upgradedOneShot);
    }

    void Reset() { xcam::Reset(); }

} // namespace net::audio::xcam_bridge
