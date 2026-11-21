// native MusicSlotSeen / MusicSlotCos producer (gated; ext remains the behaviour owner)

#include "music-sight.hpp"

#include "music-tier.hpp"

#include "log.hpp"                 // TO1_LOG must precede engine/script.hpp (template helper)
#include "engine/raycast.hpp"
#include "engine/script.hpp"

#include "net/match/match.hpp"
#include "net/match/net-player.hpp"

#include <cmath>
#include <cstdint>

namespace net::audio::music_sight {

    namespace {

        constexpr float kChestUpM    = 1.1f;    // ext: chest = locator + 1.1 m on Y
        constexpr float kConeCosMin  = 0.3f;    // ext MusicSlotCos close-quarters test
        constexpr float kRayClearT   = 0.98f;   // hit this close to the chest still counts as clear

        Vector3  g_camPos{};
        Vector3  g_camDir{};
        bool     g_camOk = false;
        bool     g_seen[kMaxNetPlayers] = {};
        float    g_cos[kMaxNetPlayers]  = {};
        uint64_t g_job[kMaxNetPlayers]  = {};

        bool SightFn(int slot, void*)
        {
            return slot >= 0 && slot < kMaxNetPlayers && g_seen[slot];
        }

        bool ConeFn(int slot, void*)
        {
            return slot >= 0 && slot < kMaxNetPlayers
                && g_cos[slot] > kConeCosMin;
        }

    }

    void Install()
    {
        if (!kEnabled)
            return;
        music_tier::SetSightSampler(&SightFn, nullptr);
        music_tier::SetConeSampler(&ConeFn, nullptr);
    }

    void Tick()
    {
        if (!kEnabled)
            return;

        g_camOk = engine::raycast::CameraPosition(g_camPos);
        if (g_camOk) {
            const Vector3* d = DC::Call<"camera-get-dir-through-reticle", const Vector3>();
            if (d && std::isfinite(d->x) && std::isfinite(d->y) && std::isfinite(d->z))
                g_camDir = *d;
            else
                g_camOk = false;
        }
        if (!g_camOk) {
            for (int slot = 0; slot < kMaxNetPlayers; ++slot)
                g_seen[slot] = false, g_cos[slot] = -2.0f, g_job[slot] = 0;
            return;
        }

        const ::net::match::Match& m = ::net::match::Get();
        const NetPlayer* local = NetPlayers::Local();
        for (int slot = 0; slot < kMaxNetPlayers; ++slot) {
            const NetPlayer& p = m.players[slot];
            if (slot == (local ? local->netSlot : -1) || !p.active || !p.hasState
                || NetPlayers::SlotIsFriendly(slot)) {
                g_seen[slot] = false;
                g_cos[slot] = -2.0f;
                g_job[slot] = 0;
                continue;
            }
            const PlayerNetState& s = p.State();
            const Vector3 chest{ s.posX, s.posY + kChestUpM, s.posZ };
            if (!std::isfinite(chest.x) || !std::isfinite(chest.y) || !std::isfinite(chest.z)) {
                g_seen[slot] = false;
                g_cos[slot] = -2.0f;
                continue;
            }

            const Vector3 v = chest - g_camPos;
            const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            const float depth = v.x * g_camDir.x + v.y * g_camDir.y + v.z * g_camDir.z;
            g_cos[slot] = len > 0.1f ? depth / len : 1.0f;

            // ext MusicSlotSeen's frustum gate (depth + forward cone) before the visibility ray.
            if (!SeenFrustumOk(depth, len)) {
                g_seen[slot] = false;
                g_job[slot] = 0;
                continue;
            }

            // Read LAST frame's job, then kick this frame's. RayT() == -1 (no result yet)
            // optimistically counts as clear; the tier's 500 ms dwell absorbs the latency.
            const float rt = engine::raycast::RayT(g_job[slot]);
            const bool occluded = rt >= 0.0f && rt < kRayClearT;
            g_seen[slot] = !occluded;
            g_job[slot] = engine::raycast::KickWorldRay(
                g_camPos, chest, engine::raycast::kMaskStaticWorld, SID("player"));
        }
    }

    void Reset()
    {
        for (int slot = 0; slot < kMaxNetPlayers; ++slot)
            g_seen[slot] = false, g_cos[slot] = -2.0f, g_job[slot] = 0;
        g_camOk = false;
    }

} // namespace net::audio::music_sight
