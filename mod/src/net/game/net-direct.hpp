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
// Honest gates: the melee bridge is live (net/match/melee-bridge.*, kEnabled = true) — its sinks
// send these exact opcodes over SendEventRaw, apply reliable hits through the normal damage path,
// and mirror local swings / dodge presses from the existing hooks. Still gated inside it: the
// private hit-react clip table, QTE, the exchange contact replay, MV3 glue and PSYNC's wire core.
// The WIRE side is ported: net-protocol.hpp pins the ext's exact melee-critical opcodes
// and Eligible() below mirrors the ext's NdTypeOk (0x6C hit-rel, 0x97..0x99 dodge, 0x9A hitreact2,
// 0x9C MEX, 0x4B PSYNC and 0x9E MV3 minus their PING / PONG) plus the two mod-native melee
// contacts (NEVT_PLAYER_MELEE_ATTACK / NEVT_TAKE_DAMAGE). The relay RTT baseline is the mod's
// per-peer PingMs() rather than the ext's g_psRtt[]. There is still no generic all-message
// send/recv hook in the mod; the two integration points are net::session::SendEventRaw (outgoing)
// and HandleState's RelayIn / HandleNetEvent pair (incoming).
//
// Cutover note: the helper remembers one loopback "ext" address from [06] hello and forwards all
// direct messages to it. Both copies hello with the same cadence while enabled, and the same
// t2r-no-direct-melee.txt turns both off, so while the ext is live the registration is shared;
// only after the ext copy is disabled is this path the sole owner. The mod now mirrors the ext's
// opcodes, so isolation comes from that single registration (one handler per machine), not from
// distinct message ids.

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
