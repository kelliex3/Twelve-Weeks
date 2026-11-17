#pragma once

#include <cstdint>

namespace gameplay {

    inline constexpr int kMaxLoadoutPoints = 13;
    inline constexpr int kMaxEquippedPerks = 3; // SkillOne / SkillTwo / SkillThree

    enum class PerkId : uint64_t {
        NONE = 0,
        HAWKEYED = 1ull << 0,
        HEADHUNTER = 1ull << 1,
        SHARPEARS = 1ull << 2,
        AGILITY = 1ull << 3,
        MARATHONRUNNER = 1ull << 4,
        STEADYHANDS = 1ull << 5,
        ADRENALINE = 1ull << 6,
        FIRSTAIDTRAINING = 1ull << 7,
		SECONDCHANCE = 1ull << 8,
        COLLECTOR = 1ull << 9,
    };

    constexpr PerkId operator|(PerkId a, PerkId b) { return static_cast<PerkId>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b)); }
    constexpr PerkId operator&(PerkId a, PerkId b) { return static_cast<PerkId>(static_cast<uint64_t>(a) & static_cast<uint64_t>(b)); }
    constexpr PerkId operator~(PerkId a) { return static_cast<PerkId>(~static_cast<uint64_t>(a)); }
    constexpr PerkId& operator|=(PerkId& a, PerkId b) { a = a | b; return a; }
    constexpr PerkId& operator&=(PerkId& a, PerkId b) { a = a & b; return a; }

    using PerkIds = PerkId;

    constexpr bool HasPerk(PerkIds set, PerkId id) {
        return static_cast<uint64_t>(set & id) != 0;
    }

    struct SpawnContext {
        bool respawn = false; // false = your first spawn of the match
    };

    struct DeathContext {
        int killerSlot = -1; // -1 = world
        uint64_t weaponSid = 0;
        bool headshot = false;
    };

    struct HitContext {
        int victimSlot = -1;
        uint64_t weaponSid = 0;
        bool headshot = false;
        bool lethal = false;
    };

    using PerkTickFn = void (*)(float dt);
    using PerkSpawnFn = void (*)(const SpawnContext&);
    using PerkDeathFn = void (*)(const DeathContext&);
    using PerkHitFn = void (*)(const HitContext&);
    using PerkResetFn = void (*)();
    using PerkPartsAwardedFn = void (*)(int parts);

    // Every callback is optional. Leave it null and the dispatcher skips it.
    // OnTick/OnSpawn/OnDeath/OnDealHit only run while the perk is equipped.
    struct Perk {
        PerkId id = PerkId::NONE;
        const char* name = "";
        const char* description = "";
        const char* icon = "hud/weapon-cross/empty-slot.svg";
        std::uint32_t lpCost = 0;

        PerkTickFn OnTick = nullptr;
        PerkSpawnFn OnSpawn = nullptr;
        PerkDeathFn OnDeath = nullptr;
        PerkHitFn OnDealHit = nullptr;
        PerkResetFn OnReset = nullptr;
        PerkPartsAwardedFn OnPartsAwarded = nullptr;

        // Tick even when the perk is not equipped. Only Hawk-eyed uses it: in 2013 Factions
        // anyone could mark enemies (the perk only made the mark last longer / glow), so the
        // marking tick must not sit behind the perk gate (was winmm_patches.py: mark-without-perk).
        bool alwaysOn = false;
    };
}