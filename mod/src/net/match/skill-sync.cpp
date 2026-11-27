// Native peer-skill owner (tools/ext/skills.c MSG_SK_SYNC). The pure rules live
// in skill-sync-core.hpp; the launcher JSON, teammate positions, clock, wire
// and slots stay here. The ext's exact byte 0x50 is pinned in net-protocol.hpp.

#include "skill-sync.hpp"

#include "boot.hpp"
#include "engine/exe_side.hpp"
#include "engine/object.hpp"
#include "net/game/player-status.hpp"
#include "net/match/net-player.hpp"
#include "net/online/net-session.hpp"
#include "net/protocol/net-protocol.hpp"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace net::match::skill_sync {

    namespace {

        namespace core = net::match::skill_sync::core;

        core::Peer g_peers[kMaxNetPlayers];
        uint8_t    g_id[core::kRows] = {};
        uint8_t    g_lv[core::kRows] = {};
        uint64_t   g_rowsAt = 0;
        uint64_t   g_sendAt = 0;

        uint64_t Now() noexcept { return GetTickCount64(); }

        // The launcher writes the ACTIVE custom loadout's four skill rows to
        // t2r-loadout-launcher.json; the game-owned t2r-loadout.json has the
        // same shape. Keep the last honest rows when neither file is readable.
        void ReadRows(uint64_t now) noexcept
        {
            if (g_rowsAt && now - g_rowsAt < core::kCacheMs)
                return;
            g_rowsAt = now ? now : 1;
            FILE* f = engine::exe_side::Open("t2r-loadout-launcher.json", "rb");
            if (!f)
                f = engine::exe_side::Open("t2r-loadout.json", "rb");
            if (!f)
                return;
            char buf[32768];
            const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = 0;
            std::fclose(f);
            core::RowsFromJson(std::string_view(buf, n), g_id, g_lv);
        }

        // ext sk_alone: Lone Wolf is active with no standing teammate within
        // 20 m XZ. Native peers are read from the same replicated world seam
        // footsteps uses; a missing local locator means "alone" (no witness).
        bool Alone() noexcept
        {
            float me[8];
            if (!engine::object::LocalPlayerWorld(me)
                || !std::isfinite(me[0]) || !std::isfinite(me[2]))
                return true;
            const int mySlot = static_cast<int>(net::session::MySlot());
            for (int slot = 0; slot < kMaxNetPlayers; ++slot) {
                if (slot == mySlot || !NetPlayers::SlotIsFriendly(slot))
                    continue;
                const NetPlayer& p = NetPlayers::BySlot(slot);
                if (!p.active || !p.spawned || p.dead || p.dying || !p.npcSid
                    || !net::player_status::PuppetAlive(slot)
                    || net::player_status::PuppetDowned(slot))
                    continue;
                float pos[8];
                if (!engine::object::EntityWorld(p.npcSid, pos)
                    || !std::isfinite(pos[0]) || !std::isfinite(pos[2]))
                    continue;
                const float dx = pos[0] - me[0], dz = pos[2] - me[2];
                if (dx * dx + dz * dz < 20.0f * 20.0f)
                    return false;
            }
            return true;
        }

    } // namespace

    void Tick()
    {
        if (boot::ExtLoaded() || !net::player_status::MatchLive()) {
            g_sendAt = 0;                  // send at once on the next match start
            return;
        }
        const uint64_t now = Now();
        ReadRows(now);
        if (g_sendAt && now - g_sendAt < core::kSendEveryMs)
            return;
        g_sendAt = now ? now : 1;
        uint8_t body[core::kMsgLen];
        core::Encode(body, g_id, g_lv,
                     core::EffectiveAgility(g_id, g_lv, Alone()));
        net::session::SendEventRaw(static_cast<uint8_t>(NEVT_SKILLS),
                                   body, static_cast<uint8_t>(sizeof body));
    }

    void OnEvent(uint8_t senderSlot, uint8_t type,
                 const uint8_t* data, uint8_t size)
    {
        if (boot::ExtLoaded() || type != static_cast<uint8_t>(NEVT_SKILLS)
            || senderSlot >= kMaxNetPlayers)
            return;
        core::Apply(g_peers[senderSlot], data, size, Now());
    }

    int AgilityLevel(int slot) noexcept
    {
        if (boot::ExtLoaded() || slot < 0 || slot >= kMaxNetPlayers)
            return 0;
        const core::Peer& p = g_peers[slot];
        return core::Fresh(p, Now()) ? core::Effective(p, core::kAgility) : 0;
    }

    int SkillLevel(int slot, int id) noexcept
    {
        if (boot::ExtLoaded() || slot < 0 || slot >= kMaxNetPlayers)
            return 0;
        const core::Peer& p = g_peers[slot];
        return core::Fresh(p, Now()) ? core::Effective(p, id) : 0;
    }

    void Reset()
    {
        for (core::Peer& p : g_peers)
            core::Reset(p);
        for (int k = 0; k < core::kRows; ++k) {
            g_id[k] = 0;
            g_lv[k] = 0;
        }
        g_rowsAt = 0;
        g_sendAt = 0;
    }

} // namespace net::match::skill_sync
