#pragma once

// Ported from tools/ext/t2r-ext.c's match / peer liveness gates (MatchSubstate, g_ffEnded,
// g_graceDead / g_graceUntil, g_puppetDeadHere, and the alive checks around them). The gates a
// net feature asks before it acts on the local player or on a remote puppet, in one place so
// every feature does not re-derive them.
//
// The downed feature is NOT ported yet, so the two downed gates below are honest stubs: they
// always answer "not downed". When downed lands, fill them in and every caller inherits it.

namespace net::player_status {

    // ---- local (sender) ------------------------------------------------------

    // In a live match that is not on the scoreboard / end screen (ext: MatchSubstate() == 6 and
    // not g_ffEnded).
    bool MatchLive();

    // The engine's own health for the local player is above zero.
    bool LocalAlive();

    // TODO(downed): the downed feature is not ported. Always false.
    bool LocalDowned();

    // The local player is in a paired move (execution / melee finisher). Approximation of the
    // ext's PsyncActive(): the mod only tracks executions, not the full paired-move set.
    bool LocalInPairedMove();

    // ---- remote (receiver) ---------------------------------------------------

    // The slot's puppet is present and alive here: active remote, spawned, not net-dead, not
    // dead locally, and outside the spawn grace. Every receiver gate in one call.
    bool PuppetAlive(int slot);

    // TODO(downed): the downed feature is not ported. Always false.
    bool PuppetDowned(int slot);

    // The slot's puppet is net-dead (a death was seen for it) or inside the respawn grace.
    bool PuppetDeadOrGrace(int slot);

    // The slot's puppet NPC is dead on THIS client (ext: g_puppetDeadHere[slot], a per-frame
    // character-is-dead? on the puppet handle). Distinct from the net death flag: a puppet we
    // killed with our own hit reads dead here before the net death arrives.
    bool PuppetDeadHere(int slot);

}
