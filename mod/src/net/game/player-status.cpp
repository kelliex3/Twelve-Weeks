// ported from tools/ext/t2r-ext.c's match / peer liveness gates

#include "player-status.hpp"

#include "net-player.hpp"
#include "match.hpp"
#include "net-gui2.hpp"

#include "net/match/downed-state.hpp"

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
        // Mirrors the ext's g_downed. The ported core is inert until downed_bridge is enabled,
        // so this reads false by default. The separate g_downDying death-clip latch is a
        // different (still gated) producer and is not part of this gate.
        return net::match::downed::IsDown();
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
        // Mirrors the ext's g_peerDowned[slot] != 0: the wire state is 0 up / 1 down / 2 dying.
        // Peer state reaches the core only while downed_bridge is enabled and NEVT_DOWNED is
        // dispatched, so this stays false by default.
        if (slot < 0 || slot >= net::match::downed::kMaxPeers)
            return false;
        return net::match::downed::PeerState(static_cast<uint8_t>(slot)) !=
               net::match::downed::kStateUp;
    }

}
