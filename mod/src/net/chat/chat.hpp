#pragma once

#include <cstdint>

// Native port of tools/ext/chat.c (build 199 + lobby chat from 200/201): the in-game
// MATCH / TEAM / PARTY / LOBBY line stores, their editor and the HUD / ESC / FIND MATCH
// panels. The pure packet / filter / reply core lives in chat-core.hpp (and its own
// standalone test); this file owns the engine seams.
//
// Wire: peer chat is NEVT_CHAT_C7 (0xC7, pinned in net-protocol.hpp) through the one
// net::session event channel. Server chat / the report-log copy are 0xD8 sub-ops 14..19
// through net::daemon::Rpc with a chat-sized 420-byte argument (friends13 keeps its own
// 100-byte arg cap; there is no shared reply registry to widen).
//
// Typing uses engine::text_input: one shared, hook-free keyboard sampler and one typing
// gate. No second WH_KEYBOARD_LL hook and no second settext hook are installed here.

namespace net::chat {

    // Once per frame from boot::FrameTick, after friends13 (party / unread state).
    void Tick(uint64_t now);

    // Called from the peer event dispatcher for NEVT_CHAT_C7. `data` is the raw event
    // payload (6..255 bytes); slot is the sender's net slot.
    void NotePeer(uint8_t slot, const uint8_t* data, uint8_t size);

    // Session reset: line stores, editor, RPC queue, dedupe and resends.
    void Reset();

} // namespace net::chat
