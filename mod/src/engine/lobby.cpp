// ported from tools/ext/t2r-ext.c (MatchSubstate / LobbyObj / the lobby offsets
// used by FeatureJoinInProgress and requeue). READ-ONLY.

#include "lobby.hpp"

#include "engine/ptr.hpp"

#include <windows.h>

namespace engine::lobby {

    namespace {

        constexpr uintptr_t kRvaNetPlayers  = 0x4BF70;        // kFnNetPlayersCtx
        constexpr uintptr_t kRvaBySlot      = 0x50FD0;        // kFnBySlot
        constexpr uintptr_t kRvaTeamOf      = 0x4C030;        // kFnTeamOf
        constexpr uintptr_t kRvaNetCtx      = 0x135388;       // kNetCtx 0x180135388

        constexpr size_t kObjBytes   = 0x2CA8;       // ext's IsBadReadPtr(lo, 0x2ca8)
        constexpr size_t kRecordBytes = 0x4C;        // ext's IsBadWritePtr(rec, 0x4c)

        uintptr_t Base()
        {
            static const uintptr_t base =
                reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
            return base;
        }

    } // namespace

    uint8_t* Object()
    {
        using FnObject = uint8_t* (*)();
        const auto fn = reinterpret_cast<FnObject>(Base() + kRvaNetPlayers);
        uint8_t* o = fn();
        if (!o || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(o), kObjBytes))
            return nullptr;
        return o;
    }

    bool Ready()
    {
        return Object() != nullptr;
    }

    uint8_t Substate()
    {
        const uintptr_t p = Base() + kRvaNetCtx + 0x2B80;
        if (!engine::ptr::Readable(p, 1))
            return 0;
        return *reinterpret_cast<volatile uint8_t*>(p);
    }

    bool InMatch()
    {
        return Substate() == kSubstateInMatch;
    }

    bool MatchOver()
    {
        const uint8_t* lo = Object();
        return lo && lo[0x2CA4] != 0;
    }

    bool Clock(float& duration, float& remaining)
    {
        const uint8_t* lo = Object();
        if (!lo)
            return false;
        duration  = *reinterpret_cast<const float*>(lo + 0x2B8C);
        remaining = *reinterpret_cast<const float*>(lo + 0x2B90);
        return true;
    }

    int RosterCount()
    {
        const uint8_t* lo = Object();
        if (!lo)
            return 0;
        const int n = lo[0x1A];
        return n > static_cast<int>(kRosterMax) ? static_cast<int>(kRosterMax) : n;
    }

    uint8_t OwnTeam()
    {
        const uint8_t* lo = Object();
        return lo ? lo[0x1C] : 0xFF;
    }

    uint8_t* RosterEntry(int index)
    {
        if (index < 0 || index >= static_cast<int>(kRosterMax))
            return nullptr;
        uint8_t* lo = Object();
        if (!lo)
            return nullptr;
        uint8_t* e = lo + 0x1D + static_cast<size_t>(index) * kEntryBytes;
        return engine::ptr::Readable(reinterpret_cast<uintptr_t>(e), kEntryBytes)
            ? e : nullptr;
    }

    uint8_t* PeerRecord(int slot)
    {
        if (slot < 0 || slot >= static_cast<int>(kRosterMax))
            return nullptr;
        using FnBySlot = uint8_t* (*)(uint32_t);
        const auto fn = reinterpret_cast<FnBySlot>(Base() + kRvaBySlot);
        uint8_t* rec = fn(static_cast<uint32_t>(slot));
        if (!rec || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(rec), kRecordBytes))
            return nullptr;
        return rec;
    }

    int TeamOf(int slot)
    {
        uint8_t* lo = Object();
        if (!lo || slot < 0 || slot >= static_cast<int>(kRosterMax))
            return -1;
        using FnTeamOf = int (*)(uint8_t*, uint32_t);
        const auto fn = reinterpret_cast<FnTeamOf>(
            reinterpret_cast<void*>(Base() + kRvaTeamOf));
        return fn(lo, static_cast<uint32_t>(slot));
    }

} // namespace engine::lobby
