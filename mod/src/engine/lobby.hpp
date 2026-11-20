#pragma once

#include <cstddef>
#include <cstdint>

// Read-only view of the engine's net lobby object, pinned to tlou-ii.exe 1.6.10721.
// Ported from tools/ext/t2r-ext.c's MatchSubstate() (kNetCtx+0x2b80), LobbyObj()
// (kFnNetPlayersCtx) and the lobby offsets used by FeatureJoinInProgress / requeue:
//
//   kNetCtx +0x2b80        match substate; 6 = in a running match
//   LobbyObj +0x1a         roster count (clamped to 8 here)
//            +0x1c         our team
//            +0x1d         8 x 0x32-byte roster entries; entry +0x31 = team
//            +0x2b8c / +0x2b90  duration / remaining clock
//            +0x2ca4        match-over byte (end banner / fade hold)
//
// This module only READS. The ext still owns the engine state while it is the bridge;
// a future cutover adds the writes next to the offsets documented above. Every pointer
// is readability-checked, so a game update or a menu-time null never faults.

namespace engine::lobby {

    constexpr size_t kRosterMax   = 8;
    constexpr size_t kEntryBytes  = 0x32;
    constexpr uint8_t kSubstateInMatch = 6;

    // Lobby object (FUN_18004BF70), nullptr when unavailable / unreadable.
    uint8_t* Object();
    bool     Ready();                 // Object() readable through +0x2ca8

    // MatchSubstate(): 0 when the data is not readable, otherwise the engine's byte.
    uint8_t  Substate();
    bool     InMatch();               // Substate() == 6

    bool     MatchOver();             // Object()[0x2ca4] != 0
    bool     Clock(float& duration, float& remaining);

    int      RosterCount();           // +0x1a clamped to kRosterMax (0 if no object)
    uint8_t  OwnTeam();               // +0x1c (0xFF if no object)
    uint8_t* RosterEntry(int index);  // +0x1d + index*0x32, nullptr if unreadable

    // winmm's net player record (FUN_180050FD0) and TeamOf (FUN_18004C030).
    uint8_t* PeerRecord(int slot);    // nullptr when the engine has no such record
    int      TeamOf(int slot);        // -1 when it cannot be answered

} // namespace engine::lobby
