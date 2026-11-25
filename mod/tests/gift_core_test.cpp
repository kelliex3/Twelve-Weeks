// Standalone harness for the native gift rules:
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/net/gift
//       mod/tests/gift_core_test.cpp -o /tmp/gift_core_test
//   /tmp/gift_core_test

#include "gift-core.hpp"

#include <cstdio>

namespace {

    using namespace net::gift::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

    void InventoryTests()
    {
        const uint64_t qty[kKindCount] = { 3, 2, 3, 4 };
        Check(Cap(0) == 3 && Cap(3) == 4 && Cap(-1) == 0,
              "the fixed caps are 3 / 3 / 3 / 4 (no Fortitude source)");
        Check(RoomMask(qty) == 0x2,
              "the room mask has only the molotov slot when the rest are full");
        Check(!HasRoom(qty[0], 0) && HasRoom(qty[1], 1) && !HasRoom(qty[3], 3),
              "the per-kind room check follows the cap");
        uint64_t full[kKindCount] = { 3, 3, 3, 4 };
        Check(RoomMask(full) == 0, "a full inventory broadcasts no room");
        Check(HasRoomInMask(0x2, 1) && !HasRoomInMask(0x2, 0),
              "a peer's mask is read one bit per kind");
    }

    void TimingTests()
    {
        Check(Fresh(9000, 1000) && !Fresh(9001, 1000) && !Fresh(1000, 0),
              "peer stock is stale after 8 s and unknown without a stamp");
        Check(RateReady(2000, 1000) && !RateReady(1999, 1000),
              "a receiver accepts one gift per slot per second");
    }

    void CraftTests()
    {
        CraftCounter c;
        uint64_t qty[kKindCount] = { 0, 0, 0, 0 };
        Check(c.Note(qty, true, 2) == -1,
              "the first craft poll only primes the inventory baseline");
        qty[0] = 1;
        Check(c.Note(qty, true, 2) == -1, "the first craft does not earn a gift");
        qty[1] = 1;
        Check(c.Note(qty, true, 2) == -1, "the second craft does not earn a gift");
        qty[2] = 1;
        Check(c.Note(qty, true, 2) == 2,
              "the third craft earns the named gift (Crafter II cadence)");
        qty[3] = 1;
        Check(c.Note(qty, true, 2) == -1, "the count restarts after a gift");
        qty[3] = 2;
        Check(c.Note(qty, false, 2) == -1,
              "a quantity rise outside the craft menu is not a craft");
        qty[0] = 3;
        c.skipNext = true;
        Check(c.Note(qty, true, 2) == -1 && !c.skipNext,
              "a gift receive is skipped once, not counted as a craft");

        CraftCounter iii;
        iii.need = 2;   // Crafter III
        uint64_t q3[kKindCount] = { 0, 0, 0, 0 };
        Check(iii.Note(q3, true, 1) == -1,
              "the III-cadence poll primes the inventory baseline too");
        q3[0] = 1;
        Check(iii.Note(q3, true, 1) == -1,
              "the first III craft does not earn a gift");
        q3[1] = 1;
        Check(iii.Note(q3, true, 1) == 1,
              "Crafter III earns a gift on the second craft");
        q3[2] = 1;
        Check(iii.Note(q3, true, 1) == -1, "the III count restarts after a gift");

        CraftCounter bad;
        bad.need = 0;
        uint64_t qb[kKindCount] = { 0, 0, 0, 0 };
        Check(bad.Note(qb, true, 1) == -1 && bad.need == 3,
              "an invalid cadence clamps back to the Crafter II default");
    }

    void ReachTests()
    {
        Check(GiverReach(1.0f, 0.5f, 0.5f),
              "the giver can hand a gift from 1.5 m away");
        Check(!GiverReach(1.6f, 0.5f, 0.0f) && !GiverReach(0.0f, 1.3f, 0.0f),
              "the giver reach is 1.5 m XZ and +-1.2 m Y");
        Check(ReceiverReach(4.0f, 2.0f, 0.0f),
              "the receiver trusts a gift from 5 m away");
        Check(!ReceiverReach(5.1f, 0.0f, 0.0f) && !ReceiverReach(0.0f, -3.1f, 0.0f),
              "the receiver reach is 5 m XZ and +-3 m Y");
    }

    void CraftLevelTests()
    {
        constexpr const char* kTwoRows =
            "{\"rev\":7,\"active\":{\"kind\":\"custom\",\"i\":1},\"custom\":["
            "{\"small\":{\"id\":\"rifle-mpx5\",\"supp\":0},\"skills\":[{\"id\":\"SK_CRAFTER\",\"lv\":2},null,null,null]},"
            "{\"small\":{\"id\":\"pistol-beretta\",\"supp\":1},\"skills\":[null,{\"id\":\"SK_CRAFTER\",\"lv\":3},null,null]}]}";
        Check(CrafterLevelFromJson(kTwoRows) == 3,
              "the saved loadout's ACTIVE custom row supplies the Crafter level");
        constexpr const char* kFirstRow =
            "{\"active\":{\"kind\":\"custom\",\"i\":0},\"custom\":["
            "{\"small\":{},\"skills\":[{\"id\":\"SK_CRAFTER\",\"lv\":2},null,null,null]},"
            "{\"small\":{},\"skills\":[{\"id\":\"SK_CRAFTER\",\"lv\":3},null,null,null]}]}";
        Check(CrafterLevelFromJson(kFirstRow) == 2,
              "a skill in another custom row does not leak into the active one");
        constexpr const char* kClass =
            "{\"active\":{\"kind\":\"class\",\"i\":0},\"custom\":["
            "{\"small\":{},\"skills\":[{\"id\":\"SK_CRAFTER\",\"lv\":3},null,null,null]}]}";
        Check(CrafterLevelFromJson(kClass) == 0,
              "a pre-made class pick has no serialized Crafter level");
        Check(CrafterLevelFromJson("{\"active\":{\"kind\":\"custom\",\"i\":9},\"custom\":[]}") == 0
              && CrafterLevelFromJson("{}") == 0,
              "a missing or out-of-range pick is an unknown Crafter level");
    }

} // namespace

int main()
{
    InventoryTests();
    TimingTests();
    CraftTests();
    CraftLevelTests();
    ReachTests();
    if (fails) {
        std::printf("%d gift core checks failed\n", fails);
        return 1;
    }
    std::puts("all gift core checks passed");
    return 0;
}
