#pragma once

#include <cstdint>

// Ported from tools/ext/netdirect.c: the direct melee path. The game's own traffic is a star
// (client -> host -> client); latency-critical events are ALSO sent straight to the peer through
// the Steam helper's loopback socket, and the relay copy still goes out. The receiver keeps the
// first copy per (sender, type, payload) in an 800 ms window; a repeat on the SAME path is a
// deliberate resend and is kept. Per-peer ping/pong over the same socket gates the path on a
// fresh pong and a direct RTT below the relay's RTT. Only a client uses it (the host already
// talks to everybody directly).
//
// Helper wire (release/client/steam/t2r-steam.h):
//   [06]                    client hello
//   [07]                    helper ack
//   [05][slot][kind][...]   data; kind 1 [type][payload], 2 [f64 t] ping, 3 [f64 t] pong
//
// The helper address comes from net::transport::RelayEndpoint(), the mod's equivalent of the
// ext's kNetCtx +0 (IP) / +0x10 (port). `t2r-no-direct-melee.txt` turns the path off.
//
// Honest gates (TODO): the ext's melee-critical producers (hit-rel 0x6C, dodge 0x97..0x9A,
// hitreact2, MEX 0x9C, MV3 0x9E, PSYNC 0x4B minus its ping/pong) are not ported, so there are
// no ext message ids to duplicate. The eligible set below is the mod's equivalent latency-
// sensitive events; the rest of the ext set stays on the ext's own direct path while it is the
// bridge. The relay RTT baseline is the mod's per-peer PingMs() rather than the ext's g_psRtt[].
// There is still no generic all-message send/recv hook in the mod; the two integration points
// are net::session::SendEventRaw (outgoing) and HandleState's HandleNetEvent call (incoming).
//
// Cutover note: the helper remembers one loopback "ext" address from [06] hello. While the ext's
// own netdirect is live it owns that registration, so this path is only exercised once the ext
// copy is disabled (the same t2r-no-direct-melee.txt file turns both off). The ext's direct
// messages use its own opcodes and never reach the mod's handler, so the two do not cross-deliver.

namespace net::direct {

    // Once a frame from boot::FrameTick.
    void Tick();

    // Session reset: close the helper socket and drop every peer measurement.
    void Reset();

    // Called from net::session::SendEventRaw after the event has been queued for the relay.
    void OnSend(uint8_t type, const void* data, uint8_t size, uint8_t targetSlot);

    // Called from the relayed receive path before replication::HandleNetEvent. Returns true
    // when that copy is a duplicate of one already delivered over the direct path, so the
    // caller skips local delivery (host forwarding of the relayed copy must still happen).
    bool RelayIn(uint8_t senderSlot, uint8_t type, const uint8_t* data, uint8_t size);

}
