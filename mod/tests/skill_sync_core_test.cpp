// Standalone harness for the pure MSG_SK_SYNC peer-skill rules; no engine or
// network headers.
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src
//       mod/tests/skill_sync_core_test.cpp -o /tmp/opencode/skill_sync_check
//   /tmp/opencode/skill_sync_check

#include "net/match/skill-sync-core.hpp"

#include <cstdio>

namespace {

    using namespace net::match::skill_sync::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

    void TableTests()
    {
        Check(kSkillCount == 33 && kRows == 4 && kMsgLen == 9,
              "the ext's skill table and pinned wire shape are unchanged");
        Check(Valid(kAgility, 2) && !Valid(kAgility, 3) && !Valid(0, 1)
              && !Valid(kSkillCount, 1),
              "a row must name a real skill at a real level");
        Check(kSkills[kAgility].nlv == 2 && kSkills[kAgility].cost[1] == 4
              && kSkills[kJack].cost[0] == 5 && kSkills[kJack].cost[1] == 10,
              "the LP costs Agility II and Jack of All Trades match the ext");
        Check(std::string_view(SkillName(kLoneWolf)) == "SK_LONEWOLF"
              && SkillName(kNone) == nullptr,
              "enum tokens map back to launcher JSON ids");
    }

    void PayloadTests()
    {
        uint8_t b[kMsgLen] = {};
        b[0] = kAgility; b[1] = kExplo;
        b[kRows + 0] = 2; b[kRows + 1] = 1;
        b[kMsgLen - 1] = 2;
        Check(PayloadValid(b, sizeof b),
              "the four rows and effective Agility byte are accepted");
        Check(PayloadValid(b, kMsgLen - 1),
              "the ext's older 8-byte message is still accepted");
        Check(AgilityByte(b, sizeof b) == 2,
              "the effective Agility byte is read from byte 8");

        b[kMsgLen - 1] = 9;
        Check(AgilityByte(b, sizeof b) == 3,
              "an impossible Agility byte is clamped to 3");

        b[kRows + 0] = 3;
        Check(!PayloadValid(b, sizeof b),
              "a level above the skill's own table is rejected");
        b[kRows + 0] = 2;
        b[0] = kSkillCount;
        Check(!PayloadValid(b, sizeof b), "an unknown skill id is rejected");
        b[0] = 0; b[kRows + 0] = 0;

        uint8_t pricey[kMsgLen] = {};
        pricey[0] = kCrafter; pricey[1] = kFat; pricey[2] = kLucky;
        pricey[kRows + 0] = 3; pricey[kRows + 1] = 3; pricey[kRows + 2] = 3;
        Check(!PayloadValid(pricey, sizeof pricey),
              "four rows costing more than 14 LP are rejected");
        Check(!PayloadValid(nullptr, sizeof b) && !PayloadValid(b, 7),
              "a truncated or absent payload is rejected");
    }

    void PeerTests()
    {
        uint8_t b[kMsgLen] = {};
        b[0] = kLoneWolf; b[kRows + 0] = 2;
        b[kMsgLen - 1] = 2;
        Peer p;
        Reset(p);
        Check(!Fresh(p, 1000), "a peer with no stamp is never fresh");
        Check(Apply(p, b, sizeof b, 1000), "the first honest message is applied");
        Check(Fresh(p, 1000) && Fresh(p, 6000) && !Fresh(p, 6001),
              "a peer is fresh for two missed 1.9 s keep-alives");
        Check(p.lv[0] == 2 && p.agility == 2,
              "the saved row and Agility byte survive the apply");

        const uint8_t bad[kMsgLen] = { kAgility, 0, 0, 0, 3, 0, 0, 0, 2 };
        Check(!Apply(p, bad, sizeof bad, 2000) && p.lv[0] == 2 && p.stamp == 1000,
              "a rejected payload never changes the saved rows");

        Check(Effective(p, kAgility) == 2,
              "Agility reads at least the sender's effective byte");
        Check(Effective(p, kSharp) == 0,
              "Lone Wolf's grants are not invented from the wire byte alone");
        Reset(p);
        Check(Effective(p, kAgility) == 0,
              "a reset peer has no effective skills");
    }

    void ExpandTests()
    {
        uint8_t eff[kSkillCount] = {};
        Expand(eff, kJack, 1, false);
        Check(eff[kBrawler] == 1 && eff[kCovert] == 1 && eff[kEars] == 1
              && eff[kStrat] == 1 && eff[kCrafter] == 1
              && eff[kExplo] == 0,
              "Jack I grants its five basic skills only");
        Expand(eff, kJack, 2, false);
        Check(eff[kExplo] == 1 && eff[kReviver] == 1 && eff[kFat] == 1
              && eff[kScav] == 1 && eff[kSecond] == 1,
              "Jack II grants the second five");

        uint8_t solo[kSkillCount] = {};
        Expand(solo, kLoneWolf, 2, true);
        Check(solo[kAgility] == 2 && solo[kSharp] == 2 && solo[kEars] == 3,
              "Lone Wolf II grants Agility II, Sharpshooter II and Sharp Ears III");
        uint8_t squad[kSkillCount] = {};
        Expand(squad, kLoneWolf, 2, false);
        Check(!squad[kAgility] && !squad[kSharp] && !squad[kEars],
              "a teammate nearby cancels every Lone Wolf grant");

        uint8_t id[kRows] = { kLoneWolf, 0, 0, 0 };
        uint8_t lv[kRows] = { 2, 0, 0, 0 };
        Check(EffectiveAgility(id, lv, true) == 2
              && EffectiveAgility(id, lv, false) == 0,
              "the sender's byte follows the alone flag from native peers");

        uint8_t wire[kMsgLen] = {};
        Encode(wire, id, lv, 7);
        Check(wire[kMsgLen - 1] == 3,
              "the encoded Agility byte is clamped on send");
    }

    void JsonTests()
    {
        constexpr const char* kLoadout =
            "{\"active\":{\"kind\":\"custom\",\"i\":1},\"custom\":["
            "{\"small\":{},\"skills\":[{\"id\":\"SK_CRAFTER\",\"lv\":3},null,null,null]},"
            "{\"small\":{},\"skills\":[{\"id\":\"SK_AGILITY\",\"lv\":2},"
            "{\"id\":\"SK_LONEWOLF\",\"lv\":2},null,"
            "{\"id\":\"SK_JACK\",\"lv\":1}]}]}";
        uint8_t id[kRows] = {}, lv[kRows] = {};
        RowsFromJson(kLoadout, id, lv);
        Check(id[0] == kAgility && lv[0] == 2 && id[1] == kLoneWolf
              && lv[1] == 2 && id[2] == kJack && lv[2] == 1 && !id[3],
              "only the ACTIVE custom row's four authored skills are read");
        Check(EffectiveAgility(id, lv, true) == 2,
              "the launcher's Lone Wolf row reaches the effective Agility feed");

        uint8_t other[kRows] = {}, otherLv[kRows] = {};
        RowsFromJson("{\"active\":{\"kind\":\"class\",\"i\":0},\"custom\":["
                     "{\"small\":{},\"skills\":[{\"id\":\"SK_AGILITY\",\"lv\":2}]}]}",
                     other, otherLv);
        Check(!other[0] && !otherLv[0],
              "a pre-made class pick has no authored skill rows");
    }

} // namespace

int main()
{
    TableTests();
    PayloadTests();
    PeerTests();
    ExpandTests();
    JsonTests();
    if (fails) {
        std::printf("%d skill-sync core checks failed\n", fails);
        return 1;
    }
    std::puts("all skill-sync core checks passed");
    return 0;
}
