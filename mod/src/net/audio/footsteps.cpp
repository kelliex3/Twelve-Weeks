// Ported from tools/ext/footsteps.c. All config / tier / gait / ankle / gain
// rules live in footsteps-core.hpp; the engine audio, sidecar, joint and network
// seams stay in this owner. No new network message.

#include "footsteps.hpp"

#include "footsteps-data.hpp"

#include "boot.hpp"
#include "engine/exe_side.hpp"
#include "engine/flag.hpp"
#include "engine/object.hpp"
#include "engine/pcm.hpp"
#include "gameplay/melee.hpp"
#include "gameplay/slowdown.hpp"
#include "log.hpp"
#include "net/game/player-status.hpp"
#include "net/match/net-player.hpp"
#include "net/match/skill-sync.hpp"
#include "net/protocol/net-protocol.hpp"

#include "sid/sid.hpp"

#include <windows.h>

// MinGW's windows.h defines `near` / `far` as macros; the core's NearCfg::near
// field and Near() helper must stay real identifiers in this translation unit.
#undef near
#undef far

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace net::audio::footsteps {

    namespace {

        namespace core = net::audio::footsteps::core;
        namespace data = net::audio::footsteps::data;

        constexpr char kPcm[] = "t2r-pcm-footsteps.txt";
        constexpr char kCfg[] = "t2r-footsteps.txt";
        constexpr uint64_t kCfgMs = 5000;

        core::Config     g_cfg;
        core::Peer       g_peers[kMaxNetPlayers];
        uint64_t         g_cfgAt = 0;
        uint32_t         g_frame = 0;
        bool             g_logged = false;

        bool ExtOwns() noexcept { return boot::ExtLoaded(); }
        bool PcmOn() noexcept { return engine::flag::Present(kPcm, 120); }

        uint64_t Now() noexcept { return GetTickCount64(); }

        void Config() noexcept
        {
            const uint64_t now = Now();
            if (g_cfgAt && now - g_cfgAt < kCfgMs)
                return;
            g_cfgAt = now;
            FILE* f = engine::exe_side::Open(kCfg, "r");
            if (!f)
                return;
            char text[4096] = {};
            const std::size_t n = std::fread(text, 1, sizeof(text) - 1, f);
            text[n] = 0;
            std::fclose(f);
            core::Parse(text, g_cfg);
            if (!g_logged) {
                g_logged = true;
                TO1_LOG("[FOOTSTEPS] t2r-footsteps.txt: %d tier(s), near %.2f %.1f-%.1f m, "
                        "speeds %.1f/%.1f/%.1f%s\n",
                        core::kTierCount, g_cfg.near.keep, g_cfg.near.lo, g_cfg.near.hi,
                        g_cfg.walk, g_cfg.run, g_cfg.sprint,
                        g_cfg.forceCadence ? ", cadence forced" : "");
            }
        }

        // The core's tier curve is the only distance model; PlayAtRange's
        // rnear/rfar (from the step) neutralize the mixer's default 3-30 m curve.
        void Play(const core::Step& s, const float* pos) noexcept
        {
            const engine::pcm::Sample* src = data::Frames(s.wave);
            if (!src)
                return;
            const data::View d = data::Data();
            if (s.wave < 0 || s.wave >= d.waveCount)
                return;
            const int n = static_cast<int>(d.waves[s.wave].n);
            if (n < 2)
                return;
            engine::pcm::PlayAtRange(src, n, s.gain, static_cast<int>(s.rate),
                                     pos, s.rnear, s.rfar);
        }

        float ListenerDistance(const float* pos) noexcept
        {
            float me[8];
            if (!engine::object::LocalPlayerWorld(me))
                return 1e9f;                 // spectating / loading: no ear
            const float dx = pos[0] - me[0], dy = pos[1] - me[1], dz = pos[2] - me[2];
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        void PeerTick(const NetPlayer& p, int slot, double now) noexcept
        {
            core::Peer& s = g_peers[slot];
            if (!p.npcSid || p.isLocal || !p.active || !p.spawned
                || p.dead || p.dying
                || !net::player_status::PuppetAlive(slot)
                || net::player_status::PuppetDeadHere(slot)
                || net::player_status::PuppetDowned(slot)
                || gameplay::melee::PuppetFrozenForExecution(slot)) {
                core::ResetPeer(s);          // ext: s->have = 0
                return;
            }

            float body[8];
            if (!engine::object::EntityWorld(p.npcSid, body)
                || !std::isfinite(body[0]) || !std::isfinite(body[1])
                || !std::isfinite(body[2])) {
                core::ResetPeer(s);
                return;
            }

            core::Input in;
            in.body = p.npcSid;
            in.nowMs = now;
            in.tick = static_cast<uint64_t>(now);
            in.slot = static_cast<uint32_t>(slot);
            in.pos[0] = body[0];
            in.pos[1] = body[1];
            in.pos[2] = body[2];
            in.havePos = true;
            in.sampleHead = ((g_frame + slot) % 4) == 0;
            in.distance = ListenerDistance(body);
            // ext FS_AGILITY2 silences an Agility II peer at every gait; its
            // effective level comes from the pinned MSG_SK_SYNC byte 8 (which
            // already includes Lone Wolf). SdMul() scales the speed thresholds.
            in.agilityLevel = net::match::skill_sync::AgilityLevel(slot);
            in.speedScale = gameplay::slowdown::Mul();

            if (in.sampleHead && engine::object::JointLocator(
                    p.npcSid, "headb", in.headPos, nullptr))
                ;                            // core reads headPos only when this is true
            if (engine::object::JointLocator(p.npcSid, "l_ankle", in.anklePos[0], nullptr)
                && engine::object::JointLocator(p.npcSid, "r_ankle", in.anklePos[1], nullptr))
                in.haveAnkles = true;

            core::Result out;
            core::Tick(s, data::Data(), g_cfg, in, out);
            for (int i = 0; i < out.count; ++i)
                Play(out.steps[i], body);
        }

    } // namespace

    void Tick()
    {
        g_frame++;
        if (ExtOwns() || !PcmOn() || !data::Have
            || !net::player_status::MatchLive()) {
            Reset();
            return;
        }
        Config();
        const double now = static_cast<double>(Now());
        for (int slot = 0; slot < kMaxNetPlayers; ++slot) {
            const NetPlayer& p = NetPlayers::BySlot(slot);
            if (!p.active || p.isLocal)
                continue;
            PeerTick(p, slot, now);
        }
    }

    bool GroundSpeed(uint8_t slot, float& out) noexcept
    {
        if (slot >= kMaxNetPlayers || !g_peers[slot].have)
            return false;
        out = g_peers[slot].v;
        return true;
    }

    bool VerticalSpeed(uint8_t slot, float& out) noexcept
    {
        if (slot >= kMaxNetPlayers || !g_peers[slot].have)
            return false;
        out = g_peers[slot].vy;
        return true;
    }

    void Reset()
    {
        for (int i = 0; i < kMaxNetPlayers; ++i)
            core::ResetPeer(g_peers[i]);
    }

} // namespace net::audio::footsteps
