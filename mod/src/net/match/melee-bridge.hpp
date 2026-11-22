#pragma once

#include <cstddef>
#include <cstdint>

// Gated integration seams for the melee logic cores ported in this batch
// (hitrel, melee-dodge, melee-hit-react, melee-exchange). The cores themselves are pure
// modules behind Sink interfaces; this bridge is the ONE place that installs those sinks and
// fans the frame tick / net-event dispatch / session reset out to them.
//
// kEnabled is true. The bridge is live: the clocks / slots / ping / match-liveness are real,
// and the transport and engine-capable sinks now send the exact reserved wire opcodes through
// net::session::SendEventRaw and apply incoming hits through the mod's normal damage path.
// Local producers feed the cores from the existing melee hooks (hitrel SendHit, hitreact
// OnBlowDealt / OnBlowTaken, dodge swing/press tracking). The ext copy stays behaviour owner for
// the effects listed under "still gated" below; its own t2r-*.txt toggles are untouched.
//
// Still gated (no native producer in this repo): the private hit-react clip table
// (melee_hitreact_rm.h) so PickClip has no sid to play; punch-at-joint / blade FX confirmation;
// the QTE system (Qte* / BeginQte / EndQte); the exchange's engine-held contact replay; MV3's
// Mv3nIo + glue; and PSYNC's wire core. Those sinks keep conservative defaults and log nothing.
//
// Wire IDs are the exact ext bytes reserved in net-protocol.hpp; no low 0..18 event is touched.

namespace net::match::melee_bridge {

    // Master gate. The wire / hit / dodge paths are live; see the gates above for what is not.
    inline constexpr bool kEnabled = true;

    // Idempotent. With kEnabled == false this is a no-op. Otherwise installs one sink per core
    // wired to the real clock / slots / ping / SendEventRaw / damage path (see the gates above).
    void Install();

    // Local producers, called from the existing melee hooks when our own swing / hit lands.
    // Both no-op while the bridge is disabled.
    void OnLocalMeleeSwing();                 // announces NEVT_DODGE_SWING (0x97)
    void OnLocalHit(uint8_t victimSlot,
                    const uint8_t damage[0x16]); // hitrel send (reliable or plain fallback)

    // Called once per frame from boot::FrameTick (after engine::frame::Tick, so NowMs is valid).
    // Also feeds the local dodge press edge (0x99) from the raw dodge pad bit.
    void Tick();

    // Called from net-replication's HandleNetEvent for the reserved melee opcodes. Each branch
    // feeds the matching core's Receive/Recv*; the MV3 / PSYNC wire cores stay gated (their
    // engine-free cores need an Mv3nIo / placement bridge that is not ported yet).
    void OnEvent(uint8_t sender, uint8_t type, const uint8_t* data, uint8_t size);

    // Called from net-replication's ResetRuntime (session reset / leaving the match).
    void Reset();

    // True once the inert sinks have been installed (diagnostics / future consumers).
    bool Installed() noexcept;

} // namespace net::match::melee_bridge
