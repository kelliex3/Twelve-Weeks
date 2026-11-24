#pragma once

#include <cstdint>

// Native icon kill feed (tools/ext/killfeed.c). Rows are hud-root widgets; the
// names are drawn from the native HUD text path. The legacy debug-draw text feed
// stays available through t2r-old-killfeed.txt and is suppressed natively when
// this icon feed is the owner (no detours are needed: the mod's text feed is our
// own producer, see net/ui/kill-feed.cpp).
namespace net::ui::killfeed {

    // ext MSG_KFWEAPON 0x94: [u8 victim][u64 weapon][u8 1 = down].
    void OnPeerWeapon(uint8_t sender, const uint8_t* data, uint8_t size);

    // Producers from the mod's own death / down events.
    // OnKill is fed by the confirmed net kill, including our own death: that is
    // where the 9-byte (kill) MSG_KFWEAPON answer is sent. NoteDownBegin/End are
    // fed by peer NEVT_DOWNED / NEVT_REVIVE. NoteLocalDown is the local-down
    // seam for the 10-byte (down) answer; the downed core's BeginDown /
    // OnDownBegin producer is shadow-mode (downed_bridge::kEnabled == false),
    // so no native caller exists yet and the kill answer is the only one sent.
    void OnKill(uint8_t victimSlot, uint8_t killerSlot, bool haveKiller,
                uint64_t weaponSID, const char* victimName, const char* killerName,
                bool victimFriendly, bool killerFriendly);
    void NoteDownBegin(uint8_t victim, uint8_t downer);
    void NoteDownEnd(uint8_t victim);
    void NoteLocalDown(uint8_t downer, uint64_t weapon);

    void Tick(uint64_t now);
    void Draw();                 // HUD name pass (game's debug-draw text)
    bool Active();               // icon feed owns the visible feed

    void Reset();

}
