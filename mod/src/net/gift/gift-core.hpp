#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

// Pure rules for tools/ext/gift.c. No engine / net headers: the bridge supplies
// inventory quantities, the clock, peer facts and the three reserved wire
// bytes (0x56 stock, 0x53 give, 0x57 ACK) defined in net-protocol.hpp.
namespace net::gift::core {

    inline constexpr int kKindCount = 4;

    // The ext's GIFT_K table: health kit, molotov, smoke bomb, nail bomb.
    inline constexpr uint64_t kItemSid[kKindCount] = {
        0xa4311670996f650aULL, 0x2a3164cb4a1c95d9ULL,
        0xa2aa922866d0479dULL, 0xcf576d288d014eacULL
    };

    inline constexpr uint64_t kGiftSid = 0xd015ce722b6330efULL; // FNV64("gift")

    inline constexpr uint64_t kStockStaleMs = 8000;
    inline constexpr uint64_t kStockBroadcastMs = 2500;
    inline constexpr uint64_t kPendingTimeoutMs = 2000;
    inline constexpr uint64_t kReceiverRateMs = 1000;

    inline constexpr float kGiverXZ = 1.5f;
    inline constexpr float kGiverY = 1.2f;
    inline constexpr float kReceiverXZ = 5.0f;
    inline constexpr float kReceiverY = 3.0f;

    // The ACTIVE custom loadout row in the launcher's saved JSON
    // (t2r-loadout-launcher.json; the game-owned t2r-loadout.json has the same
    // shape). Empty for an absent file, a pre-made "class" pick or an
    // unparsable active index. Shared by the Crafter gift producer and the
    // skill-sync producer so both read the same four skill rows.
    inline std::string_view ActiveCustomLoadout(std::string_view s) noexcept
    {
        const size_t active = s.find("\"active\"");
        if (active == std::string_view::npos)
            return {};
        const size_t aEnd = s.find('}', active);
        // Match the KEY: the active pick itself has `"kind":"custom"`.
        const size_t custom = s.find("\"custom\":", active);
        if (aEnd == std::string_view::npos || custom == std::string_view::npos ||
            aEnd > custom)
            return {};
        // A pre-made "class" pick has no serialized skill rows.
        if (s.find("\"class\"", active) < aEnd)
            return {};

        size_t idx = 0;
        const size_t iPos = s.find("\"i\"", active);
        if (iPos != std::string_view::npos && iPos < aEnd) {
            const size_t colon = s.find(':', iPos);
            if (colon != std::string_view::npos && colon < aEnd) {
                size_t p = colon + 1;
                while (p < aEnd && (s[p] == ' ' || s[p] == '\t')) ++p;
                if (p < aEnd && s[p] >= '0' && s[p] <= '9') {
                    int v = 0;
                    while (p < aEnd && s[p] >= '0' && s[p] <= '9') {
                        v = v * 10 + (s[p++] - '0');
                        if (v > 63) { v = -1; break; }
                    }
                    if (v >= 0) idx = static_cast<size_t>(v);
                }
            }
        }

        // Every custom row starts with {"small":; walk to the active one and
        // bound the scan to it so a skill in another loadout is ignored.
        size_t obj = s.find("\"small\"", custom);
        for (size_t k = 0; k < idx && obj != std::string_view::npos; ++k)
            obj = s.find("\"small\"", obj + 1);
        if (obj == std::string_view::npos)
            return {};
        const size_t next = s.find("\"small\"", obj + 1);
        return s.substr(obj, (next == std::string_view::npos ? s.size() : next) - obj);
    }

    // Level of one named skill (e.g. "SK_CRAFTER") in that row; 0 = unknown.
    inline int SkillLevelFromJson(std::string_view row, std::string_view id) noexcept
    {
        if (id.empty())
            return 0;
        const size_t at = row.find('"' + std::string(id) + '"');
        if (at == std::string_view::npos)
            return 0;
        const size_t lv = row.find("\"lv\"", at);
        if (lv == std::string_view::npos)
            return 0;
        const size_t colon = row.find(':', lv);
        if (colon == std::string_view::npos)
            return 0;
        size_t p = colon + 1;
        while (p < row.size() && (row[p] == ' ' || row[p] == '\t')) ++p;
        if (p >= row.size() || row[p] < '1' || row[p] > '3')
            return 0;
        return row[p] - '0';
    }

    inline int SkillLevelFromJson(std::string_view row, const char* id) noexcept
    {
        return id ? SkillLevelFromJson(row, std::string_view(id)) : 0;
    }

    // Crafter II / III level of the ACTIVE custom loadout. 0 = unknown, which
    // maps to the conservative Crafter II (every 3rd) cadence.
    inline int CrafterLevelFromJson(std::string_view s) noexcept
    {
        return SkillLevelFromJson(ActiveCustomLoadout(s), "SK_CRAFTER");
    }

    inline constexpr bool ValidKind(int kind) noexcept
    {
        return kind >= 0 && kind < kKindCount;
    }

    // The engine's own carry caps. Fortitude II's extra health kit is not
    // sourced natively, so the fixed 2013 cap stays at 3 for every kit build.
    inline constexpr int Cap(int kind) noexcept
    {
        return ValidKind(kind) && kind == 3 ? 4 : ValidKind(kind) ? 3 : 0;
    }

    inline constexpr bool HasRoom(uint64_t quantity, int kind) noexcept
    {
        return ValidKind(kind) && quantity < static_cast<uint64_t>(Cap(kind));
    }

    inline uint8_t RoomMask(const uint64_t quantity[kKindCount]) noexcept
    {
        uint8_t mask = 0;
        for (int k = 0; k < kKindCount; ++k)
            if (HasRoom(quantity[k], k))
                mask |= static_cast<uint8_t>(1u << k);
        return static_cast<uint8_t>(mask & 0xFu);
    }

    inline constexpr bool HasRoomInMask(uint8_t mask, int kind) noexcept
    {
        return ValidKind(kind) && (mask & static_cast<uint8_t>(1u << kind)) != 0;
    }

    inline constexpr bool Fresh(uint64_t now, uint64_t stamp,
                                uint64_t staleMs = kStockStaleMs) noexcept
    {
        return stamp != 0 && now >= stamp && now - stamp <= staleMs;
    }

    // GiftGive's partner search: nearest standing teammate inside 1.5 m XZ
    // and +-1.2 m Y. The LOS / teammate / up checks belong to the bridge.
    inline bool GiverReach(float dx, float dy, float dz) noexcept
    {
        if (!std::isfinite(dx + dy + dz))
            return false;
        const float xz = dx * dx + dz * dz;
        return xz < kGiverXZ * kGiverXZ && dy > -kGiverY && dy < kGiverY;
    }

    // GiftRecv's trust check on the receiving side (5 m XZ, +-3 m Y).
    inline bool ReceiverReach(float dx, float dy, float dz) noexcept
    {
        if (!std::isfinite(dx + dy + dz))
            return false;
        const float xz = dx * dx + dz * dz;
        return xz < kReceiverXZ * kReceiverXZ && dy > -kReceiverY && dy < kReceiverY;
    }

    inline constexpr bool RateReady(uint64_t now, uint64_t last,
                                    uint64_t gapMs = kReceiverRateMs) noexcept
    {
        return last == 0 || now - last >= gapMs;
    }

    // Native craft producer. The bridge observes real quantity increases of
    // craftable items while its own backpack page is open; the caller supplies
    // the cadence from the configured Crafter level (II = every 3rd, III =
    // every 2nd) and the random gift kind. This state machine only decides
    // WHEN one is earned, and ignores man-made quantity jumps (a gift receive)
    // through `skipNext`.
    struct CraftCounter {
        uint64_t prev[kKindCount]{};
        bool     primed = false;
        bool     skipNext = false;
        int      count = 0;
        int      need = 3;   // Crafter II cadence; the caller sets 2 for III

        // Returns the supplied kind on the craft that earns a gift, else -1.
        int Note(const uint64_t qty[kKindCount], bool inCraftMenu,
                 int randomKind) noexcept
        {
            if (need < 2)
                need = 3;           // a bad caller never awards faster than Crafter II
            if (!primed || skipNext) {
                primed = true;
                skipNext = false;
                for (int k = 0; k < kKindCount; ++k)
                    prev[k] = qty[k];
                return -1;
            }
            bool crafted = false;
            if (inCraftMenu)
                for (int k = 0; k < kKindCount; ++k)
                    if (qty[k] > prev[k])
                        crafted = true;
            for (int k = 0; k < kKindCount; ++k)
                prev[k] = qty[k];
            if (!crafted || !ValidKind(randomKind))
                return -1;
            if (++count < need)
                return -1;
            count = 0;
            return randomKind;
        }
    };

} // namespace net::gift::core
