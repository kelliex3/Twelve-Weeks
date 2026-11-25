#pragma once

#include <cstdint>

// Native owner of the Crafter gift wire (tools/ext/gift.c) when t2r-ext.dll is
// absent. boot::ExtLoaded() is the only cutover: every send, inventory write,
// award and prompt feed here no-ops while the ext is loaded.
//
// Wire bytes are the ext's exact values from net-protocol.hpp:
//   NEVT_GIFT_STOCK 0x56 [u8 roomMask & 0xF]        to everyone
//   NEVT_GIFT       0x53 [u8 kind]                  giver -> receiver
//   NEVT_GIFT_ACK   0x57 [u8 1 taken / 0 refused]   receiver -> giver
// No hook, detour, toggle or low net event is added.
namespace net::gift {

    inline constexpr bool kEnabled = true;

    void Install();
    void Tick();
    void OnEvent(uint8_t sender, uint8_t type, const uint8_t* data, uint8_t size);
    void Reset();
    bool Installed() noexcept;

    // Native crafting producers replace the ext's sk_crafter_gift: craft-menu
    // quantity edges call NoteCrafted with the cadence from the saved loadout
    // (Crafter III = every 2nd, else every 3rd), and a 0 -> 1 gift item edge
    // calls NoteGiftKind. NoteCrafted grants the box and queues it when one is
    // carried.
    void NoteGiftKind(int kind);
    void NoteCrafted(int kind);

} // namespace net::gift
