#pragma once

#include <cstdint>

// Ported from tools/ext/t2r-ext.c's match / peer liveness gates (MatchSubstate, g_ffEnded,
// g_graceDead / g_graceUntil, g_puppetDeadHere, and the alive checks around them). The gates a
// net feature asks before it acts on the local player or on a remote puppet, in one place so
// every feature does not re-derive them.
//
// The downed state core is ported (net/match/downed-state.*) and the two gates below read it.
// Its integration bridge is dormant by default (net::match::downed_bridge::kEnabled == false),
// so until a producer is cut over the core stays in its "up" state and these gates answer
// The local ext dying latch (g_downDying / g_uc4DyingUntil) is carried by the same core now, so
// the 0 / 1 / 2 down-state accessors can distinguish crawling from dying. No producer is cut
// over yet: until a death clip feeds the latch, LocalState() answers 1 while down, never 2.

namespace net::player_status {

    // ---- local (sender) ------------------------------------------------------

    // In a live match that is not on the scoreboard / end screen (ext: MatchSubstate() == 6 and
    // not g_ffEnded).
    bool MatchLive();

    // The engine's own health for the local player is above zero.
    bool LocalAlive();

    // The local player is in the ext's downed state (g_downed). Live only when the downed bridge
    // is enabled and a producer feeds the core.
    bool LocalDowned();

    // bloodbuild / feel's 0 / 1 / 2 answer: 0 up, 1 down (crawling), 2 dying. Reads the same
    // core as LocalDowned() plus the (still unproduced) local dying latches.
    uint8_t LocalDownState();

    // The local player is in a paired move (execution / melee finisher). Approximation of the
    // ext's PsyncActive(): the mod only tracks executions, not the full paired-move set.
    bool LocalInPairedMove();

    // ---- remote (receiver) ---------------------------------------------------

    // The slot's puppet is present and alive here: active remote, spawned, not net-dead, not
    // dead locally, and outside the spawn grace. Every receiver gate in one call.
    bool PuppetAlive(int slot);

    // The slot's peer is in the ext's MSG_DOWNED state (0 up / 1 down / 2 dying; g_peerDowned
    // != 0), i.e. down or dying. Live only when the downed bridge is enabled and peer wire
    // events are dispatched.
    bool PuppetDowned(int slot);

    // The ext's MSG_DOWNED state for a peer: 0 up / 1 down / 2 dying (g_peerDowned[slot]).
    // The wire already carries the distinction, so this is faithful whenever the bridge feeds
    // NEVT_DOWNED; it is 0 by default while the bridge is dormant.
    uint8_t PuppetDownState(int slot);

    // The slot's puppet is net-dead (a death was seen for it) or inside the respawn grace.
    bool PuppetDeadOrGrace(int slot);

    // The slot's puppet NPC is dead on THIS client (ext: g_puppetDeadHere[slot], a per-frame
    // character-is-dead? on the puppet handle). Distinct from the net death flag: a puppet we
    // killed with our own hit reads dead here before the net death arrives.
    bool PuppetDeadHere(int slot);

}
