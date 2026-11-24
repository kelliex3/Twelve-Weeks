#pragma once

#include <cstdint>

// Port of tools/ext/loadseq.c: the TWELVE WEEKS entry sequence on the connecting
// page (black fade, slow title fade + stinger, thin progress bar / title fill,
// hand-off fade through the live menu, menu-world resume re-push).
//
// Sidecars keep the ext's meaning: t2r-no-load-seq.txt = old instant-fill
// behaviour, t2r-no-load-stinger.txt = silent, t2r-load-fill.txt = title-clipped
// fill instead of the default bottom bar. No new mod-style toggle was added.
namespace menu::loadseq {

    void Tick(uint64_t now);
    void Reset();

    bool Active();        // a connecting page is being driven
    bool HoldActive();    // g_ldsHold: the hand-off title is covering the menu

}
