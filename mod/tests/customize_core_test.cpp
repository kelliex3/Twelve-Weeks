// Standalone harness for the CUSTOMIZE profile/wire core:
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/net/ui
//       mod/tests/customize_core_test.cpp -o /tmp/customize_core_test
//   /tmp/customize_core_test
//
// The profile format is append-only and shipped to the daemon on the same
// bytes as the build-191 format; these checks pin both ends.

#include "customize-core.hpp"

#include <cstdio>
#include <cstring>

namespace {

    using namespace net::ui::customize::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

    void FormatTests()
    {
        Check(kVer == 2 && kWearCats == 7 && kMsgLen == 4 + 2 * (1 + kWearCats),
              "the file and wire versions keep the build-191 sizes");
        Profile p;
        Defaults(p);
        char buf[16384];
        const int n = Format(p, buf, sizeof buf);
        Check(n > 6 && std::strncmp(buf, "T2RCZ1", 6) == 0,
              "the saved file starts with the version header");

        Profile q;
        Check(Parse(q, buf) == 1, "a freshly written profile parses back");
        Check(q.sex[0] == p.sex[0] && q.it[1][1][3] == p.it[1][1][3],
              "sex and worn item indexes survive the round-trip");
        Check(Parse(q, "no header") == 0, "a foreign file is rejected");
        Check(TableId() != 0, "the item-table id is stable for the daemon");

        p.flags = kFlagUonly;
        const int n2 = Format(p, buf, sizeof buf);
        Check(n2 > 6 && std::strstr(buf, "flag uonly 1\n") != nullptr,
              "the UNLOCKED ONLY flag round-trips without changing the format");
        Profile r;
        Check(Parse(r, buf) == 1 && (r.flags & kFlagUonly) != 0,
              "the UNLOCKED ONLY flag survives a parse");
        Check(kFlagCount == 2 && std::strcmp(kFlagKeys[1], "uonly") == 0,
              "the flag key table keeps its append-only slot");
    }

    void WearTests()
    {
        const char* txt =
            "T2RCZ1\n"
            "side wlf m\n"
            "look wlf m 0 0 0 0 0 0 0\n"
            "wear wlf m hat TAN RAINCOAT\n";
        Profile p;
        Check(Parse(p, txt) == 1, "the label-wins file parses");
        Check(p.sex[0] == 0, "the side line sets the faction's sex");
    }

    void FilterTests()
    {
        constexpr int fac = 0, sex = 0, cat = CZ_TORSO;
        Profile p;
        Defaults(p);
        const int total = Count(fac, sex, cat);
        int locked = -1;
        for (int k = 0; k < total; ++k) {
            const cz_item_t* it = Item(fac, sex, cat, k);
            if (!it || k == p.it[fac][sex][cat] || Unlisted(it, false, false)
                || it->uk <= 0 || it->uk >= CZ_NCHAL
                || PlainKind(CZ_CHAL[it->uk].kind) || Unlocked(p, it))
                continue;
            locked = k;
            break;
        }
        Check(locked >= 0, "a grantable locked row exists in the WLF male torso list");
        if (locked < 0) return;
        const int all = Listed(p, fac, sex, cat, false, false);
        Check(all > 0 && all <= total, "the unfiltered list counts the offered rows");
        if (!kUonlyEnabled) {
            p.flags = kFlagUonly;
            Check(Listed(p, fac, sex, cat, false, false) == all,
                  "a build without the UNLOCKED ONLY gate lists every offered row");
            return;
        }

        p.flags = kFlagUonly;
        const int shown = Listed(p, fac, sex, cat, false, false);
        Check(shown > 0 && shown < all, "UNLOCKED ONLY hides locked rows");
        Check(RowOut(p, fac, sex, cat, locked, false, false),
              "a locked row is filtered out");
        Check(!RowOut(p, fac, sex, cat, p.it[fac][sex][cat], false, false),
              "the worn row stays listed");

        p.it[fac][sex][cat] = static_cast<uint8_t>(locked);
        Check(!RowOut(p, fac, sex, cat, locked, false, false),
              "a worn locked row stays listed so the grid keeps its check");
        const int row = K2R(p, fac, sex, cat, locked, false, false);
        Check(row >= 0 && row < Listed(p, fac, sex, cat, false, false),
              "the worn locked row has a valid focus position");
        Check(R2K(p, fac, sex, cat, row, false, false) == locked,
              "row/key conversion round-trips with the filter on");

        bool sexOpen = Listed(p, fac, sex, CZ_SEX, false, false) == Count(fac, sex, CZ_SEX);
        for (int k = 0; k < Count(fac, sex, CZ_SEX); ++k)
            sexOpen = sexOpen && !RowOut(p, fac, sex, CZ_SEX, k, false, false);
        Check(sexOpen, "SEX rows are never filtered by UNLOCKED ONLY");

        Profile q;
        Defaults(q);
        q.flags = kFlagUonly;
        const cz_item_t* it = Item(fac, sex, cat, locked);
        Check(RowOut(q, fac, sex, cat, locked, false, false),
              "a fresh profile filters the locked row");
        Check(GrantAdd(q, it->label), "a grant line unlocks the item's label");
        Check(!GrantAdd(q, it->label), "the same grant is not added twice");
        Check(Unlocked(q, it) && !RowOut(q, fac, sex, cat, locked, false, false),
              "a granted row is listed with UNLOCKED ONLY on");
    }

    void WireTests()
    {
        Profile p;
        Defaults(p);
        const uint8_t hat = static_cast<uint8_t>(Count(0, 0, 0) - 1);
        p.it[0][0][0] = hat;
        p.it[0][0][1] = 0;
        uint8_t msg[kMsgLen];
        const int n = Msg(p, 7, msg);
        Check(n == kMsgLen && msg[3] == 7, "Msg emits the fixed-size wire payload");

        Wire out;
        Check(Recv(msg, n, out) == 1, "Recv accepts the fixed-size payload");
        Check(out.counter == 7 && out.it[0][0] == hat && out.it[0][1] == 0,
              "the wire round-trip keeps the counter and worn indexes");
        Check(Recv(msg, n - 1, out) == 0, "a short wire frame is rejected");
    }

    void ItemTests()
    {
        Check(CZ_NFAC == 2 && CZ_NSEX == 2 && CZ_NCAT == 8 && CZ_NCHAL == 65,
              "the generated item tables keep their build-191 dimensions");
        Check(Count(0, 0, 0) > 0 && Item(0, 0, 0, 0) != nullptr,
              "the first faction / sex / category has a table");
        Check(Item(0, 0, 0, -1) == nullptr, "an out-of-range item is not offered");
        char name[64];
        Check(LookName(0, 1, 2, name, sizeof name) && name[0] == 't',
              "look names follow the t2r-cz-<fac><sex>-<body> form");
    }

} // namespace

int main()
{
    FormatTests();
    WearTests();
    FilterTests();
    WireTests();
    ItemTests();
    if (fails) {
        std::printf("%d failure(s)\n", fails);
        return 1;
    }
    std::printf("customize core: all checks passed\n");
    return 0;
}
