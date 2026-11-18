#pragma once

#include <cstdint>

// Ported from tools/ext/glass.c: synced breakable glass / windows. Destruction is local
// by default -- a window shot or smashed on one screen stays whole on the others -- so
// this feature mirrors it over the mod's own event transport.
//
// How it works: every event the engine sends to a breakable ends in the base handler at
// exe RVA 0x84a820 (SimpleBreakable vtable +0x70; BigBreakable tail-jumps to it). The
// handler is inline-hooked. A break event (hash 0x2ccc09d462355d58) seen on OUR screen is
// turned into MSG_GLASS carrying the pane's id (obj+0xc0), the broken piece index (event
// arg 2) and the event's three args. The receiver finds that pane (the engine's own
// lookup, else a registry of the panes the hook has seen) and replays the same event
// through the engine's event sender (RVA 0x15734d0), so the pane runs its own break
// handler: state, fx, sound, collision off. A replay sets a flag so the hook never echoes
// it back; a piece already broken here (our own physics got there first) is skipped. A
// peer that joins (slot turns active) gets the list of panes broken on our screen.
//
// Opt-out: t2r-no-glass-sync.txt next to the exe.
//
// Wire (MSG_GLASS 0x7A, pinned to the ext's byte so the two interoperate during cutover):
//   [u8 1][u64 pane id][u32 piece][u8 nargs][nargs x 16B event arg slots]   one break
//   [u8 2][u8 n][n x ([u64 pane id][u32 piece])]                           panes broken here
//
// Tick() runs once per frame from boot::FrameTick (it installs the hook lazily and drives
// the joiner list); OnEvent() runs on the game thread from net::replication::HandleNetEvent.

namespace net::glass {

    void Tick();       // ext GlassTick
    void OnEvent(uint8_t senderSlot, const uint8_t* data, uint8_t size);   // ext GlassRecv, applied directly

    // Session teardown / leave the match: drop the pane registry, the fast cache and the
    // joiner state so a new session re-learns and re-sends.
    void Reset();

}
