// Standalone harness for the CUSTOMIZE compose cache / planning core:
//   g++ -std=c++20 -Wall -Wextra -Werror -pedantic -I mod/src/net/ui
//       mod/tests/customize_compose_core_test.cpp -o /tmp/opencode/customize_compose_core_test
//   /tmp/opencode/customize_compose_core_test
//
// The wire stays version 2 / 18 bytes / NEVT_CUSTOM_LOOK 0x59; the tests pin
// the peer-cache and look2 planning decisions without any engine dependency.

#include "customize-compose-core.hpp"

#include <cstdio>
#include <cstring>

namespace {

    using namespace net::ui::customize::compose::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

    void PoolAndWireTests()
    {
        int rows[8]{};
        Check(PoolRows(1, 0, rows) == 6 && rows[0] == 0 && rows[5] == 5,
              "Seraphite male pool rows are 0..5");
        Check(PoolRows(0, 0, rows) == 6 && rows[0] == 6 && rows[5] == 11,
              "WLF male pool rows are 6..11");
        Check(PoolRows(1, 1, rows) == 2 && rows[0] == 12 && rows[1] == 13,
              "Seraphite female pool rows are 12 and 13");
        Check(PoolRows(0, 1, rows) == 2 && rows[0] == 14 && rows[1] == 15,
              "WLF female pool rows are 14 and 15");
        Check(PoolRows(0, 0, nullptr) == 0 && PoolRows(2, 0, rows) == 0,
              "invalid pool requests return no rows");

        WireCache c;
        Wire w{};
        w.ok = 1;
        w.sex[0] = 1;
        w.it[0][CZ_TORSO] = 7;
        const bool changed = WireChanged(c, w, "Alpha");
        Check(changed, "an unseen slot is always a change");
        c.wire = w;
        std::snprintf(c.name, sizeof c.name, "Alpha");
        Check(!WireChanged(c, w, "Alpha"), "the same wire and name are not a change");
        Check(WireChanged(c, w, "Beta"), "a renamed slot invalidates the cached wire");
        w.it[0][CZ_TORSO] = 8;
        Check(WireChanged(c, w, "Alpha"), "edited items are a change");
        Check(WireCurrent(c, "Alpha") && WireCurrent(c, "")
              && WireCurrent(WireCache{}, "Alpha") && !WireCurrent(c, "Beta"),
              "an empty name on either side keeps the cache, a different name drops it");
    }

    void OutfitTests()
    {
        Profile p;
        Defaults(p);
        p.it[0][0][CZ_TORSO] = 5;

        WireCache c;
        c.wire.ok = 1;
        c.wire.sex[0] = 0;
        c.wire.it[0][CZ_TORSO] = 9;
        std::snprintf(c.name, sizeof c.name, "Peer");

        uint8_t out[kWearCats]{};
        Check(OutfitFor(p, c, true, false, 0, 0, out)
              && out[CZ_TORSO] == p.it[0][0][CZ_TORSO],
              "our body reads the live profile, not the wire cache");
        Check(OutfitFor(p, c, false, true, 0, 0, out) && out[CZ_TORSO] == 9,
              "a puppet reads its slot's cached wire");
        Check(!OutfitFor(p, c, false, false, 0, 0, out),
              "a slot with no valid wire cannot compose");
        c.wire.sex[0] = 1;
        Check(OutfitFor(p, c, false, true, 0, 0, out)
              && out[CZ_TORSO] == CZ_DEFAULT[0][0][CZ_TORSO],
              "a sex mismatch falls back to that faction/sex defaults");
        Check(!OutfitFor(p, c, true, false, 2, 0, out),
              "invalid faction/sex indexes are refused");
    }

    void MaterialTests()
    {
        char name[kNameLen];
        Check(FaceRecordName("t2r-cz-wm-5", 3, name, sizeof name)
              && std::strcmp(name, "t2r-cz-wm-5-3") == 0,
              "face records are named '<body>-<face index>'");
        Check(FaceRecordIndex(1, 0, 7) == 7 && FaceRecordIndex(0, 0, 2) == 1
              && FaceRecordIndex(0, 1, 3) == 3 && FaceRecordIndex(0, 0, 13) == 0,
              "WLF faces resolve to their skin-tone group, Seraphite faces are unchanged");
        Check(FaceRecordName("t2r-cz-wm-5", -1, name, sizeof name) == false
              && !FaceRecordName("t2r-cz-wm-5", 3, name, 8),
              "face record names refuse bad indexes and truncation");

        int slot = -1;
        Check(PaletteSlot(0, 4, slot) && slot == 2,
              "face 0 uses the first per-face palette slot");
        Check(PaletteSlot(3, 4, slot) && slot == 5,
              "face 3 uses pa[5]");
        Check(!PaletteSlot(4, 4, slot) && !PaletteSlot(0, 0, slot),
              "palette slots outside nface are refused");

        Check(FaceVoice(0, 0, 0) == -1,
              "male bodies do not force a female voice");
        Check(FaceVoice(0, 1, 12) == 2,
              "KELLIE keeps voice C in every outfit");
        Check(FaceVoice(0, 1, 0) == -1,
              "other faces leave the pool voice alone");
    }

    void HairTests()
    {
        // FACE 02 of WLF males (cz_s51): a head, drawn bare hair and a
        // per-hat hair, plus a hidden part that must not be listed.
        Part sup[4];
        uint8_t keep[4] = {1, 1, 1, 1};
        sup[0] = { "t2r-fh-m-h1e-head-02", 1 };
        sup[1] = { "t2r-fh-m-h1e-02-hair-bare---a", 0 };
        sup[2] = { "t2r-fh-m-h1e-02-hh-wlfcap---x", 0 };
        sup[3] = { "t2r-fh-m-x1e-02-hh-patch----x", 1 };
        char pairs[kPairMax][kPartNameLen]{};
        char hair[kHairMax][kPartNameLen];
        char fit[kPartNameLen], stock[kPartNameLen];

        const int n = HairPlan(0, 0, 0, sup, keep, 4, pairs, 0, hair, fit, stock);
        Check(n == 2 && std::strcmp(hair[0], "t2r-fh-m-h1e-02-hair-bare---a") == 0
              && std::strcmp(hair[1], "t2r-fh-m-h1e-02-hh-wlfcap---x") == 0,
              "only hair parts that the outfit draws are listed");
        Check(fit[0] == 0 && stock[0] == 0,
              "a hidden stock head with no second head needs no swap");

        // FACE 04 of WLF males is list index 2 (cz_s53): stock + hat-fitted second head.
        Part hs[2];
        uint8_t hk[2] = {1, 1};
        hs[0] = { "militia-male-head-04", 1 };
        hs[1] = { "t2r-hh--male-head-04", 0 };
        const int hn = HairPlan(0, 0, 2, hs, hk, 2, pairs, 0, hair, fit, stock);
        Check(hn == 0 && std::strcmp(fit, "t2r-hh--male-head-04") == 0
              && std::strcmp(stock, "militia-male-head-04") == 0,
              "a hat-fitted head swaps with the hidden stock head, not listed as hair");

        // Its pushed armour pair already replaces the stock head: no swap.
        char paired[kPairMax][kPartNameLen]{};
        std::snprintf(paired[0], kPartNameLen, "%s", "militia-male-head-04");
        const int pn = HairPlan(0, 0, 2, hs, hk, 2, paired, 1, hair, fit, stock);
        Check(pn == 1 && std::strcmp(hair[0], "t2r-hh--male-head-04") == 0
              && fit[0] == 0,
              "a paired stock head lets the second head be listed normally");
    }

    void BodyTests()
    {
        Body b;
        Check(RowOfBody(b, 0) == -1 && RowOfBody(b, 1) == -1 && RowOfBody(b, 2) == -1,
              "an uncomposed body has no pool row");
        b.on = true;
        b.rowTorso = 6;
        b.rowLower = 4;
        b.rowBag = 5;
        Check(RowOfBody(b, 0) == 6 && RowOfBody(b, 1) == 4
              && RowOfBody(b, 2) == 5 && RowOfBody(b, 3) == -1,
              "a composed body reports torso / lower / bag rows");
    }

} // namespace

int main()
{
    std::printf("customize compose core:\n");
    PoolAndWireTests();
    OutfitTests();
    MaterialTests();
    HairTests();
    BodyTests();
    if (fails) {
        std::printf("customize compose core: %d check(s) failed\n", fails);
        return 1;
    }
    std::printf("customize compose core: all checks passed\n");
    return 0;
}
