// ported from tools/ext/t2r-ext.c's match / peer liveness gates

#include "player-status.hpp"

#include "net-player.hpp"
#include "match.hpp"
#include "net-gui2.hpp"

#include "engine/local-player.hpp"
#include "gameplay/melee.hpp"

#include "log.hpp"

namespace net::player_status {

    bool MatchLive()
    {
        return net::gui2::InMatch() && !net::gui2::ScoreboardShowing();
    }

    bool LocalAlive()
    {
        return engine::local_player::Health() > 0.0f;
    }

    bool LocalDowned()
    {
        // TODO(downed): the downed feature is not ported. When it lands, return the local
        // downed latch here (the ext's g_downed / g_downDying).
        return false;
    }

    bool LocalInPairedMove()
    {
        // Approximation of the ext's PsyncActive(): an execution we are committed to, either
        // side. (CommittedToExecutor() is true while being executed too.)
        return gameplay::melee::CommittedToExecutor();
    }

    bool PuppetDeadOrGrace(int slot)
    {
        if (slot < 0 || slot >= kMaxNetPlayers)
            return true;
        const net::match::SlotMatchState& ss = net::match::Get().slotState[slot];
        return ss.dead || ss.respawnGrace > 0.0f;
    }

    bool PuppetAlive(int slot)
    {
        if (slot < 0 || slot >= kMaxNetPlayers)
            return false;
        const NetPlayer& p = NetPlayers::BySlot(slot);
        if (!p.active || p.IsLocal() || !p.spawned || p.dead)
            return false;
        if (PuppetDeadOrGrace(slot))
            return false;
        // Last, because it costs a native call: the puppet may be dead here (killed by our
        // own hit) before the net death flag reaches us.
        return !PuppetDeadHere(slot);
    }

    bool PuppetDeadHere(int slot)
    {
        if (slot < 0 || slot >= kMaxNetPlayers)
            return false;
        const StringId64 h = NetPlayers::BySlot(slot).npcSid;
        return h != 0 && DC::Call<"character-is-dead?">(h) != 0;
    }

    bool PuppetDowned(int slot)
    {
        // TODO(downed): the downed feature is not ported. When it lands, return that slot's
        // downed latch here (the ext's g_peerDowned[slot]).
        (void)slot;
        return false;
    }

}
