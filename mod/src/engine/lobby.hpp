#pragma once

#include <cstddef>
#include <cstdint>

// Read-only view of the engine's net lobby object, pinned to tlou-ii.exe 1.6.10721.
// Ported from tools/ext/t2r-ext.c's MatchSubstate() (kNetCtx+0x2b80), LobbyObj()
// (kFnNetPlayersCtx) and the lobby offsets used by FeatureJoinInProgress / requeue:
//
//   kNetCtx +0x2b80        match substate; 6 = in a running match
//   kPartyStatus           raw engine session state (exe 0x180167968): 0 idle,
//                          1 connecting, 2 connected, 3 searching, 4 in a match
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

    // ext T2R_SYM(kPartyStatus) -- the raw int the ext reads for the lobby / party
    // rows (3 = searching / match found, 4 = in a match). -1 if not readable.
    int      PartyStatus();
    uint8_t* RawObject();             // the same read-only LobbyObj() as Object()

    bool     MatchOver();             // Object()[0x2ca4] != 0
    bool     Clock(float& duration, float& remaining);

    int      RosterCount();           // +0x1a clamped to kRosterMax (0 if no object)
    uint8_t  OwnTeam();               // +0x1c (0xFF if no object)
    uint8_t* RosterEntry(int index);  // +0x1d + index*0x32, nullptr if unreadable

    // winmm's net player record (FUN_180050FD0) and TeamOf (FUN_18004C030).
    uint8_t* PeerRecord(int slot);    // nullptr when the engine has no such record
    int      TeamOf(int slot);        // -1 when it cannot be answered

    // ext LobbyRowsSorted (t2r-ext.c): the MATCH FOUND roster order -- our team first
    // (blue), the enemy after (red), each in slot order, our own slot skipped. Row 0
    // is always us; rows 1.. are idx[k] / enemy[k]. -1 when there is no usable match
    // record yet (searching / menu), so a caller can tell match-found from searching
    // without guessing a team. idx / enemy must each hold kRosterMax ints.
    int  RosterSorted(int idx[kRosterMax], int enemy[kRosterMax]);

    // The shown name of a roster entry (entry +1, NUL-terminated, at most 48 chars).
    // false / out[0] = 0 when the entry is unavailable.
    bool RosterName(int index, char* out, size_t cap);

    // ---- native owner of winmm's spectate fields -----------------------------------
    // The death screen owns these now (ext wrote +0x2c44/+0x2c48/+0x2c4c/+0x2c50/
    // +0x2c54 and the UI refresh flag). Every write is VirtualQuery-checked, so a
    // game update / menu-time null fails closed.
    constexpr size_t kSpecOn      = 0x2C44;
    constexpr size_t kSpecTimer   = 0x2C48;
    constexpr size_t kSpecTarget  = 0x2C4C;
    constexpr size_t kSpecSlot2   = 0x2C50;
    constexpr size_t kSpecFlag    = 0x2C54;
    constexpr uintptr_t kUiRefreshFlagVa = 0x180162914;
    constexpr float kSpecPeriodSecs = 3.1f;  // winmm's 3 s period + 0.1

    bool SpectateState(bool& on, float& timer, int& target);
    bool SpectateBegin();                       // enable with "pick first target" defaults
    bool SpectateEnd();                         // disable
    bool SpectateSetTarget(int slot);           // switch: target + timer past the period
    void SpectateRefreshUi();                   // zero the engine's UI refresh flag

} // namespace engine::lobby
