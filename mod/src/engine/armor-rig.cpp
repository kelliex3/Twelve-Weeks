// ported from tools/ext/t2r-ext.c (the skinned-armour section: ArmorRigOn / DcLookExists /
// ArmorRigTwinIdx / ArmorRigTwinName / ArmorRigSwapField / ArmorRigMeshPresent / GvisDc / GvisRead /
// GvisSet / ArmorBodyAt / ArmorRigBody / ArmorRigFailed / ArmorRigApply / WtwinInit /
// ArmorWinterTwinRow / ArmorWinterWlfK / ArmorWinterTwinName)

#include "armor-rig.hpp"

#include "backpack-prop.hpp"
#include "net/ui/customize-compose.hpp"
#include "exe_side.hpp"
#include "frame.hpp"
#include "object.hpp"
#include "ptr.hpp"

#include "log.hpp"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace engine::armor_rig {

    namespace {

        uintptr_t ExeBase()
        {
            return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        }

        // ext AR_LOG (cap 200).
        int g_arLog;
        #define AR_LOG(...) do { if (g_arLog < 200) { g_arLog++; TO1_LOG(__VA_ARGS__); } } while (0)

        // ---- the look tables (ext ARMOR_RIG_*) --------------------------------------------

        const char* const kBase[kRigN] = {
            "scar-male-13", "scar-male-01", "scar-male-12", "scar-male-06",
            "scar-male-09", "scar-male-02",
            "militia-male-11-backpack", "militia-male-08-backpack",
            "militia-male-12-backpack",
            "militia-male-05-backpack", "militia-male-13-backpack",
            "militia-male-14-backpack",
            "scar-female-11", "scar-female-09", "militia-female-13-backpack",
            "militia-female-12-backpack",
        };

        const char* const kClash[kRigN] = {      // part hidden while the helmet shows (look2 mesh names)
            "scar-male-hoodup", "scar-male-hoodup", "scar-male-hoodup",
            "scar-male-hoodup", "scar-male-hoodup", "scar-male-hoodup",
            nullptr, "militia-male-hat-08", nullptr, nullptr, nullptr,
            "militia-male-hat-14",
            "scar-female-hoodup", "scar-female-hoodup",
            "militia-female-hat-13", nullptr,
        };

        const char* const kMesh[2]  = { "slaver-male-helmet-02", "slaver-vest-04-torso" };
        const char* const kMeshF[2] = { "slaver-fhelm1-rattler-helmet-02", "slaver-fvest1-01-torso" };

        // ext SCAR_BODY / WLF_BODY / SCAR_BODY_F / WLF_BODY_F (the pushed body copies).
        #define SCAR_BODY(j, head) \
            { "scar-male-" j "-jacket-torso", "scar-arm1-" j "-jacket-torso", 2 }, \
            { "scar-male-head-" head, "scar-arm1-head-" head, 1 }, \
            { "scar-male-hooddown", "scar-arm1-hooddown", 3 }, \
            { "scar-t2r-bp-rigged", "scar-t2r-bp-armour", 2 }
        #define WLF_BODY(n, bag, bagc) \
            { "militia-male-" n "-torso", "militia-arm1-" n "-torso", 2 }, \
            { "militia-male-head-" n, "militia-arm1-head-" n, 1 }, \
            { bag, bagc, 2 }, { nullptr, nullptr, 0 }
        #define SCAR_BODY_F(head) \
            { "scar-female-a-jacket-torso", "scar-farm01-a-jacket-torso", 2 }, \
            { "scar-female-head-" head, "scar-farm01-head-" head, 1 }, \
            { "scar-female-hooddown", "scar-farm01-hooddown", 3 }, \
            { "scar-female-backpack", "scar-farm01-backpack", 2 }
        #define WLF_BODY_F(n) \
            { "militia-female-" n "-torso", "militia-farm01-" n "-torso", 2 }, \
            { "militia-female-head-" n, "militia-farm01-head-" n, 1 }, \
            { "militia-female-backpack-" n, "militia-farm01-backpack-" n, 2 }, \
            { nullptr, nullptr, 0 }

        const BodyPair kBody[kRigN][kBodyN] = {  // per ARMOR_RIG_BASES
            { SCAR_BODY("a", "13") }, { SCAR_BODY("b", "01") },
            { SCAR_BODY("a", "12") }, { SCAR_BODY("c", "06") },
            { SCAR_BODY("b", "09") }, { SCAR_BODY("a", "07") },
            { WLF_BODY("11", "militia-t2r-bp-rigged-11", "militia-t2r-bp-armour-11") },
            { WLF_BODY("08", "militia-male-backpack-08", "militia-arm1-backpack-08") },
            { WLF_BODY("12", "militia-t2r-bp-rigged-12", "militia-t2r-bp-armour-12") },
            { WLF_BODY("05", "militia-male-backpack-05", "militia-arm1-backpack-05") },
            { WLF_BODY("13", "militia-male-backpack-13", "militia-arm1-backpack-13") },
            { WLF_BODY("14", "militia-male-backpack-14", "militia-arm1-backpack-14") },
            { SCAR_BODY_F("11") }, { SCAR_BODY_F("09") },
            { WLF_BODY_F("13") }, { WLF_BODY_F("12") },
        };

        #undef SCAR_BODY
        #undef WLF_BODY
        #undef SCAR_BODY_F
        #undef WLF_BODY_F

        // The WLF winter looks wear other clothes than their base look: their own pushed copies
        // (ext ARMOR_WINTER_BODY), used instead of kBody[row] while the body wears that winter twin.
        #define WINTER_BODY(torso, tcopy, head, hcopy, bag, bagc) \
            { { torso, tcopy, 2 }, { head, hcopy, 1 }, { bag, bagc, 2 }, { nullptr, nullptr, 0 } }
        const BodyPair kWinterBody[8][kBodyN] = {   // WLF pool index k = 0..7
            WINTER_BODY("owen-t2r-winter-01", "owen-t2r-pushed-01",
                        "militia-male-head-07", "militia-arm1-head-07",
                        "militia-t2r-bp-rigged-11", "militia-t2r-bp-armour-11"),
            WINTER_BODY("manny-t2r-winter-01", "manny-t2r-pushed-01",
                        "militia-male-head-10", "militia-arm1-head-10",
                        "militia-male-backpack-08", "militia-arm1-backpack-08"),
            WINTER_BODY("jordan-t2r-winter-01", "jordan-t2r-pushed-01",
                        "militia-male-head-07", "militia-arm1-head-07",
                        "militia-t2r-bp-rigged-12", "militia-t2r-bp-armour-12"),
            WINTER_BODY("jackson-male-torso-02", "jackson-arm1-torso-02",
                        "militia-male-head-10", "militia-arm1-head-10",
                        "militia-male-backpack-05", "militia-arm1-backpack-05"),
            WINTER_BODY("jackson-male-torso-08", "jackson-arm1-torso-08",
                        "militia-male-head-07", "militia-arm1-head-07",
                        "militia-male-backpack-13", "militia-arm1-backpack-13"),
            WINTER_BODY("jackson-male-torso-04", "jackson-arm1-torso-04",
                        "militia-male-head-10", "militia-arm1-head-10",
                        "militia-male-backpack-14", "militia-arm1-backpack-14"),
            WINTER_BODY("jackson-female-torso-02", "jackson-farm01-torso-02",
                        "militia-female-head-08", "militia-farm01-head-08",
                        "militia-female-backpack-13", "militia-farm01-backpack-13"),
            WINTER_BODY("jackson-female-torso-02", "jackson-farm01-torso-02",
                        "militia-female-head-12", "militia-farm01-head-12",
                        "militia-female-backpack-12", "militia-farm01-backpack-12"),
        };
        #undef WINTER_BODY

        // ext FACTION_LOOKS / FACTION_LOOKS_WINTER ([pool][idx]).
        const char* const kFactionLook[2][kFlN] = {
            { "scar-male-13", "scar-male-01", "scar-male-12", "scar-male-06",
              "scar-male-09", "scar-male-02", "scar-female-11", "scar-female-09" },
            { "militia-male-11-backpack", "militia-male-08-backpack",
              "militia-male-12-backpack", "militia-male-05-backpack",
              "militia-male-13-backpack", "militia-male-14-backpack",
              "militia-female-13-backpack", "militia-female-12-backpack" },
        };
        const char* const kFactionWinter[2][kFlN] = {
            { "scar-winter-13", "scar-winter-01", "scar-winter-12", "scar-winter-06",
              "scar-winter-09", "scar-winter-02", "scar-winter-f11", "scar-winter-f09" },
            { "wlf-winter-01", "wlf-winter-02", "wlf-winter-03", "wlf-winter-05",
              "wlf-winter-13", "wlf-winter-14", "wlf-winter-f13", "wlf-winter-f12" },
        };

        // ---- decision / twins --------------------------------------------------------------

        int      g_armorRig = -1;             // -1 not decided yet, 0 off, 1 on
        uint64_t g_twin[kRigN];
        uint64_t g_nextTry;
        int      g_tries;

        int ArmorRigFileOff()
        {
            return engine::exe_side::Exists("t2r-no-armor-rig.txt") ? 1 : 0;
        }

        // ext WtwinInit: the winter looks' armoured twins answer as the row of the pool look they
        // replace.
        uint64_t g_wtwin[2][kFlN];
        int      g_wtwinRow[2][kFlN];
        int      g_wtwinDone;

        void WtwinInit();

        int WinterTwinRowRaw(uint64_t sid)
        {
            WtwinInit();
            for (int t = 0; t < 2; t++)
                for (int k = 0; k < kFlN; k++)
                    if (g_wtwin[t][k] && g_wtwin[t][k] == sid)
                        return g_wtwinRow[t][k];
            return -1;
        }

        int WinterWlfKRaw(uint64_t look)
        {
            WtwinInit();
            for (int k = 0; look && k < kFlN; k++)
                if (g_wtwin[1][k] && g_wtwin[1][k] == look)
                    return k;
            return -1;
        }

        int WinterTwinNameRaw(const char* look, char* out)
        {
            WtwinInit();
            for (int t = 0; t < 2; t++)
                for (int k = 0; k < kFlN; k++)
                    if (g_wtwin[t][k]
                        && std::strncmp(look, kFactionWinter[t][k], 0x28) == 0) {
                        std::snprintf(out, 0x28, "%s%s", look, kSuffix);
                        return 1;
                    }
            return 0;
        }

        void WtwinInit()
        {
            if (g_wtwinDone)
                return;
            for (int t = 0; t < 2; t++)
                for (int k = 0; k < kFlN; k++) {
                    g_wtwin[t][k] = 0;
                    g_wtwinRow[t][k] = -1;
                    if (!kFactionWinter[t][k] || !kFactionLook[t][k])
                        continue;
                    for (int i = 0; i < kRigN; i++)
                        if (std::strcmp(kFactionLook[t][k], kBase[i]) == 0)
                            g_wtwinRow[t][k] = i;
                    char tw[64];
                    std::snprintf(tw, sizeof tw, "%s%s", kFactionWinter[t][k], kSuffix);
                    if (g_wtwinRow[t][k] >= 0
                        && g_twin[g_wtwinRow[t][k]]
                        && LookExists(tw) == 1)
                        g_wtwin[t][k] = SID(tw);
                }
            g_wtwinDone = 1;
            AR_LOG("armor rig: winter armoured twins %s\n",
                (g_wtwin[1][0] || g_wtwin[0][0])
                    ? "loaded"
                    : "not in this build (winter looks wear no helmet / vest)");
        }

        // ---- garment part visibility (ext GvisDc / GvisRead / GvisSet) ---------------------

        struct GvisEnt { uint64_t chr, sid; int8_t last; uint8_t nth; };
        GvisEnt g_gvis[192];
        int g_gvisN, g_gvisNlog;

        // 1 = the draw control has an instance with that sid; *cur = the visible state the engine's
        // is-mesh-visible? would answer; *noop = set-mesh-visible(vis) would change nothing.
        int GvisRead(const uint8_t* dc, uint64_t sid, int vis, int* cur, int* noop)
        {
            const uint32_t n = *reinterpret_cast<const uint32_t*>(dc + 0x14);
            const uint8_t* a = *reinterpret_cast<uint8_t* const*>(dc + 8);
            if (!n || n > 256 || !a
                || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(a),
                    static_cast<size_t>(n) * 0x390))
                return 0;
            int found = 0, same = 1;
            for (uint32_t i = 0; i < n; i++) {
                const uint8_t* in = a + static_cast<size_t>(i) * 0x390;
                if (*reinterpret_cast<const uint64_t*>(in + 0x28) != sid)
                    continue;
                if (!found)
                    *cur = in[0x384] != 0;      // first instance with the sid
                found = 1;
                if (in[0x384] != (vis ? 0xff : 0) || in[0x385] != in[0x384]
                    || in[0x387] != in[0x386])
                    same = 0;
            }
            *noop = same;
            return found;
        }

        // ext GvisSet: read the part's visibility, compare with what we set last time (a difference =
        // the engine or a re-instance changed it: logged), then show / hide it.
        void GvisSet(int idx, uint64_t chr, const char* name, int vis, const char* who)
        {
            const uint64_t sid = SID(name);
            int k = -1;
            for (int i = 0; i < g_gvisN; i++)
                if (g_gvis[i].chr == chr && g_gvis[i].sid == sid) {
                    k = i;
                    break;
                }
            if (k < 0) {
                if (g_gvisN >= static_cast<int>(sizeof g_gvis / sizeof *g_gvis))
                    g_gvisN = 0;        // (level change: forgotten wholesale)
                k = g_gvisN++;
                g_gvis[k].chr = chr;
                g_gvis[k].sid = sid;
                g_gvis[k].last = -1;
                g_gvis[k].nth = 0;
            }
            uint8_t* dc = engine::object::DrawControl(chr);
            int cur = -1, noop = 0, fast = 0;
            if (dc)
                fast = GvisRead(dc, sid, vis != 0, &cur, &noop);
            if (!fast && dc) {
                const int mi = engine::object::MeshIndex(dc, sid);
                // no instance: the native answers "visible"
                cur = mi >= 0 ? (engine::object::MeshHidden(dc, mi) ? 0 : 1) : 1;
            }
            if (cur >= 0 && g_gvis[k].last >= 0 && cur != g_gvis[k].last
                && g_gvisNlog++ < 600)
                TO1_LOG("garment: %s%d %s %s -> %s by something else (the engine / a re-instance; the ext had set it %s) (frame %llu)\n",
                    idx == kUs ? "own" : "slot ", idx == kUs ? 0 : idx, name,
                    g_gvis[k].last ? "shown" : "hidden", cur ? "shown" : "hidden",
                    g_gvis[k].last ? "shown" : "hidden",
                    static_cast<unsigned long long>(engine::frame::Count()));
            if (cur != vis && g_gvisNlog++ < 600)
                TO1_LOG("garment: %s%d %s %s -> %s, asked by %s (frame %llu)\n",
                    idx == kUs ? "own" : "slot ", idx == kUs ? 0 : idx, name,
                    cur < 0 ? "?" : cur ? "shown" : "hidden",
                    vis ? "shown" : "hidden", who,
                    static_cast<unsigned long long>(engine::frame::Count()));
            if (fast && noop && (++g_gvis[k].nth & 31)) {
                g_gvis[k].last = static_cast<int8_t>(vis != 0);
                return;     // nothing the native would change
            }
            if (dc) {
                if (vis)
                    engine::object::ShowMesh(dc, sid);
                else
                    engine::object::HideMesh(dc, sid);
            }
            g_gvis[k].last = static_cast<int8_t>(vis != 0);
        }

        // ext ArmorRigMeshPresent: hide the part, ask - still "visible" = the mesh is not on this
        // character. `restore` re-shows it after the probe.
        int MeshPresent(uint64_t chr, const char* mesh, int restore)
        {
            uint8_t* dc = engine::object::DrawControl(chr);
            if (!dc)
                return 0;
            const uint64_t sid = SID(mesh);
            const int mi = engine::object::MeshIndex(dc, sid);
            if (mi < 0)
                return 0;
            engine::object::HideMesh(dc, sid);
            const int present = engine::object::MeshHidden(dc, mi) ? 1 : 0;
            if (restore)
                engine::object::ShowMesh(dc, sid);
            return present;
        }

        // ---- per-body state ----------------------------------------------------------------

        struct RigT {
            uint64_t key;
            int      shown, fail, clashSaved, probed;
            int      bodyOk[kBodyN];
            uint64_t refresh;
        };
        RigT g_rig[kBodies];
        int  g_rigWk[kBodies];          // per body: its WLF winter index k, else -1
        uint8_t g_pupOff[kSlots];       // ext g_bpPupOff (backpack.c)
        uint8_t g_hoodUp[kBodies];      // ext g_hoodUp (mpsnow.c)
        int  g_ownWant = -1, g_ownLook = -1;

    }

    // ========================================================================================

    const char* Base(int i)
    {
        return (i >= 0 && i < kRigN) ? kBase[i] : nullptr;
    }

    const char* FactionLook(int pool, int idx)
    {
        return (pool >= 0 && pool < 2 && idx >= 0 && idx < kFlN) ? kFactionLook[pool][idx] : nullptr;
    }

    const char* FactionWinter(int pool, int idx)
    {
        return (pool >= 0 && pool < 2 && idx >= 0 && idx < kFlN) ? kFactionWinter[pool][idx] : nullptr;
    }

    const BodyPair* BodyAt(int idx, int look_i, int k)
    {
        if (k < 0 || k >= kBodyN)
            return nullptr;
        // A composed CUSTOMIZE body carries its own pushed / original pairs;
        // ext's cz_body_pair wins over the pool look's static table.
        if (idx >= 0) {
            const char* orig = net::ui::customize::compose::BodyPairOrig(idx, k);
            if (orig) {
                static BodyPair composed{};
                composed.orig = orig;
                composed.copy = net::ui::customize::compose::BodyPairCopy(idx, k);
                composed.mask = net::ui::customize::compose::BodyPairMask(idx, k);
                return &composed;
            }
            // ext cz_body_pair(): a composed body with no pushed pair at k
            // returns an empty pair, not the pool look's static table.
            if (net::ui::customize::compose::HasComposedBody(idx)) {
                static const BodyPair none{};
                return &none;
            }
        }
        if (idx >= 0 && idx < kBodies && g_rigWk[idx] >= 0)
            return &kWinterBody[g_rigWk[idx]][k];
        if (look_i < 0 || look_i >= kRigN)
            return nullptr;
        return &kBody[look_i][k];
    }

    bool FactionLooksOn()
    {
        return engine::exe_side::Exists("t2r-faction-looks.txt") ? true : false;
    }

    // ext cz_rec(): the typed look2 record behind a name. The resolver is a
    // three-argument DC lookup (sid, "look2", default) with a type tag before
    // the record; the ext checked the same 15-byte prologue.
    uint8_t* LookRecord(const char* name)
    {
        if (!name || !name[0]) return nullptr;
        typedef uint64_t (*fn_lookup3)(uint64_t, uint64_t, uint64_t);
        static const uint8_t expect[15] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57,
            0x48, 0x83, 0xEC, 0x20
        };
        uint8_t* f = reinterpret_cast<uint8_t*>(ExeBase()) + 0x13578a0;
        if (!engine::ptr::Readable(reinterpret_cast<uintptr_t>(f), sizeof expect)
            || std::memcmp(f, expect, sizeof expect) != 0)
            return nullptr;
        const uint64_t sid = SID(name), tLook2 = SID("look2");
        uint64_t* p = reinterpret_cast<uint64_t*>(
            reinterpret_cast<fn_lookup3>(f)(sid, tLook2, 0));
        if (!p || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(p - 1), 24)
            || p[-1] != tLook2 || p[0] != sid)
            return nullptr;
        return reinterpret_cast<uint8_t*>(p);
    }

    // 1 = a look2 record with that name is loaded, 0 = not, -1 = can't tell (lookup prologue moved).
    int LookExists(const char* name)
    {
        if (!name) return -1;
        typedef uint64_t (*fn_lookup3)(uint64_t, uint64_t, uint64_t);
        static const uint8_t expect[15] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57,
            0x48, 0x83, 0xEC, 0x20
        };
        uint8_t* f = reinterpret_cast<uint8_t*>(ExeBase()) + 0x13578a0;
        if (!engine::ptr::Readable(reinterpret_cast<uintptr_t>(f), sizeof expect))
            return -1;
        if (std::memcmp(f, expect, sizeof expect) != 0)
            return -1;
        const uint64_t sid = SID(name), tLook2 = SID("look2");
        uint64_t* p = reinterpret_cast<uint64_t*>(
            reinterpret_cast<fn_lookup3>(f)(sid, tLook2, 0));
        if (!p || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(p - 1), 24)
            || p[-1] != tLook2 || p[0] != sid)
            return 0;
        const char* nm = reinterpret_cast<const char*>(p[1]);
        return (nm && engine::ptr::Readable(reinterpret_cast<uintptr_t>(nm), 48)
            && std::strcmp(nm, name) == 0) ? 1 : 0;
    }

    bool On()
    {
        if (g_armorRig >= 0)
            return g_armorRig != 0;
        if (ArmorRigFileOff()) {
            g_armorRig = 0;
            AR_LOG("armor rig: OFF (t2r-no-armor-rig.txt) - rigid attach\n");
            return false;
        }
        const uint64_t now = GetTickCount64();
        if (now < g_nextTry)
            return false;
        g_nextTry = now + 2000;
        const int base = LookExists(kBase[0]);      // checks the lookup itself
        if (base < 0) {
            g_armorRig = 0;
            AR_LOG("armor rig: OFF (look2 lookup prologue mismatch) - rigid attach\n");
            return false;
        }
        if (!base) {            // looks not resolvable yet: keep trying, rigid attach meanwhile
            if (!(g_tries++ % 30))
                AR_LOG("armor rig: base look %s not found yet (try %d) - retrying, rigid attach meanwhile\n",
                    kBase[0], g_tries);
            return false;
        }
        int ok = 1;
        for (int i = 0; i < kRigN; i++) {
            char tw[64];
            std::snprintf(tw, sizeof tw, "%s%s", kBase[i], kSuffix);
            g_twin[i] = SID(tw);
            if (LookExists(tw) != 1) {
                if (i >= kRigMaleN) {       // the female twins are optional
                    g_twin[i] = 0;
                    AR_LOG("armor rig: female armoured look %s is not loaded - that look keeps the rigid attach\n", tw);
                } else {
                    AR_LOG("armor rig: armoured look %s is not loaded\n", tw);
                    ok = 0;
                }
            }
        }
        g_armorRig = ok;
        AR_LOG(ok
            ? "armor rig: on (%d armoured looks loaded; helmet + vest are skinned look parts)\n"
            : "armor rig: OFF (this build has no armoured looks) - rigid attach\n", kRigN);
        return ok != 0;
    }

    int TwinIdx(uint64_t look)
    {
        if (g_armorRig != 1 || !look)
            return -1;
        if (const int composed = net::ui::customize::compose::RowOfLook(look, 0);
            composed >= 0)
            return composed;
        for (int i = 0; i < kRigN; i++)
            if (g_twin[i] == look)
                return i;
        return WinterTwinRowRaw(look);
    }

    int TwinName(const char* look, char* out)
    {
        if (!look || !*look || !On())
            return 0;
        const size_t n = strnlen(look, 0x28), sl = std::strlen(kSuffix);
        if (n >= 0x28)
            return 0;
        if (n > sl && std::strcmp(look + n - sl, kSuffix) == 0) {
            std::memcpy(out, look, n + 1);
            return 1;
        }
        for (int i = 0; i < kRigN; i++)
            if (g_twin[i] && std::strcmp(look, kBase[i]) == 0) {
                std::snprintf(out, 0x28, "%s%s", look, kSuffix);
                return 1;
            }
        return WinterTwinNameRaw(look, out);
    }

    int SwapField(char* fld, char saved[0x28], const char* who, uint32_t slot)
    {
        static char tw[0x28];
        if (!fld || !TwinName(fld, tw))
            return 0;
        std::memcpy(saved, fld, 0x28);
        std::memset(fld, 0, 0x28);
        std::strcpy(fld, tw);
        AR_LOG("armor rig: %s slot %u spawns as %s\n", who, slot, tw);
        return 1;
    }

    int WinterWlfK(uint64_t look)
    {
        return WinterWlfKRaw(look);
    }

    int WinterTwinRow(uint64_t look)
    {
        return WinterTwinRowRaw(look);
    }

    bool Failed(int idx, uint64_t key)
    {
        return idx >= 0 && idx < kBodies && g_rig[idx].key == key && g_rig[idx].fail >= 3;
    }

    void BodyApply(int idx, uint64_t chr, int look_i, int want, const int* ok)
    {
        for (int k = 0; k < kBodyN; k++) {
            const BodyPair* b = BodyAt(idx, look_i, k);
            if (!b || !b->orig || !ok[k])
                continue;
            const int on = (want & b->mask) != 0;
            if (((engine::backpack_prop::WornHidden() && chr == SID("player"))
                 || (idx >= 0 && idx < kSlots && g_pupOff[idx]))
                && (std::strstr(b->copy, "backpack") || std::strstr(b->copy, "bp-"))) {
                GvisSet(idx, chr, b->copy, 0, "the TAB backpack prop (worn bag off)");
                GvisSet(idx, chr, b->orig, 0, "the TAB backpack prop (worn bag off)");
                continue;
            }
            if (idx >= 0 && idx < kBodies && g_hoodUp[idx]
                && std::strstr(b->orig, "hooddown")) {   // winter hood up: neither hood-down part
                GvisSet(idx, chr, b->copy, 0, "armour rig (winter hood up: hood down copy off)");
                GvisSet(idx, chr, b->orig, 0, "armour rig (winter hood up: hood down off)");
                continue;
            }
            GvisSet(idx, chr, b->copy, on,
                on ? "armour rig (piece over it shows: pushed copy on)"
                   : "armour rig (piece over it off: pushed copy off)");
            GvisSet(idx, chr, b->orig, !on,
                on ? "armour rig (piece over it shows: original off)"
                   : "armour rig (piece over it off: original on)");
        }
    }

    void Apply(int idx, uint64_t chr, uint64_t key, int want, int look_i)
    {
        if (idx < 0 || idx >= kBodies)
            return;
        RigT* r = &g_rig[idx];
        g_rigWk[idx] = WinterWlfK(engine::object::Look(chr));   // winter twin: its own pushed clothes
        const char* const* mesh = look_i >= kRigMaleN ? kMeshF : kMesh;
        if (r->key != key) {
            std::memset(r, 0, sizeof *r);
            r->key = key;
            r->shown = -1;
        }
        if (idx == kUs) {
            g_ownWant = want;
            g_ownLook = look_i;
        }
        if (r->fail >= 3)
            return;
        if (!r->probed) {       // once per character: both parts must really be mesh instances on it
            r->probed = 1;
            const int ph = MeshPresent(chr, mesh[0], 0);
            const int pv = MeshPresent(chr, mesh[1], 0);
            if (!ph || !pv) {
                r->fail = 3;
                AR_LOG("armor rig: %s%d look %s%s has NO %s%s%s mesh instance (draw control) - rigid attach instead\n",
                    idx == kUs ? "own" : "slot ", idx == kUs ? 0 : idx,
                    look_i >= 0 ? kBase[look_i] : "?", kSuffix,
                    ph ? "" : "helmet", (!ph && !pv) ? " + " : "", pv ? "" : "vest");
                return;
            }
            AR_LOG("armor rig: %s%d helmet + vest mesh instances present\n",
                idx == kUs ? "own" : "slot ", idx == kUs ? 0 : idx);
            if (look_i >= 0) {
                char miss[256] = "";
                for (int k = 0; k < kBodyN; k++) {      // pushed body copies (hidden at spawn anyway)
                    const BodyPair* b = BodyAt(idx, look_i, k);
                    if (!b || !b->orig)
                        continue;
                    r->bodyOk[k] = MeshPresent(chr, b->copy, 0);
                    if (!r->bodyOk[k])
                        std::snprintf(miss + std::strlen(miss),
                            sizeof miss - std::strlen(miss), " %s", b->copy);
                }
                AR_LOG("armor rig: %s%d body copies%s%s\n",
                    idx == kUs ? "own" : "slot ", idx == kUs ? 0 : idx,
                    *miss ? " MISSING (original kept):" : " all present", miss);
            }
        }
        if (look_i >= 0)
            BodyApply(idx, chr, look_i, want, r->bodyOk);
        for (int k = 0; k < 2; k++)
            GvisSet(idx, chr, mesh[k], (want >> k) & 1,
                k ? "armour rig (vest)" : "armour rig (helmet)");
        const char* clash = net::ui::customize::compose::BodyClash(
            idx, look_i >= 0 ? kClash[look_i] : nullptr);
        const int helm = want & 1, wasHelm = r->shown > 0 && (r->shown & 1);
        if (clash) {            // the hood / hat the helmet clashes with: hidden while the HELMET shows
            if (helm) {
                if (!wasHelm) {
                    uint8_t* dc = engine::object::DrawControl(chr);
                    const int mi = dc ? engine::object::MeshIndex(dc, SID(clash)) : -1;
                    r->clashSaved = (mi < 0) ? 1
                        : (engine::object::MeshHidden(dc, mi) ? 0 : 1);
                }
                GvisSet(idx, chr, clash, 0, "armour rig (helmet on: hood / hat off)");
            } else if (wasHelm)
                GvisSet(idx, chr, clash, r->clashSaved, "armour rig (helmet off: hood / hat restored)");
        }
        // A composed outfit's separate hair parts are hidden while the helmet
        // shows, then restored on every pass without it (ext cz_body_hair).
        // Only parts the outfit actually draws are listed, so no saved state.
        uint8_t* dc = engine::object::DrawControl(chr);
        for (int hk = 0; ; ++hk) {
            const char* hp = net::ui::customize::compose::BodyHair(idx, hk);
            if (!hp) break;
            if (helm) {
                GvisSet(idx, chr, hp, 0, "armour rig (helmet on: hair part off)");
                continue;
            }
            const int mi = dc ? engine::object::MeshIndex(dc, SID(hp)) : -1;
            if (mi >= 0 && engine::object::MeshHidden(dc, mi))
                GvisSet(idx, chr, hp, 1, "armour rig (no helmet: hair part on)");
        }
        // The crown hat's fitted head swaps with the stock head under the
        // helmet; never leave both off (ext cz_body_head_swap).
        const char* fit = nullptr;
        const char* stock = nullptr;
        if (net::ui::customize::compose::BodyHeadSwap(idx, &fit, &stock)) {
            if (helm) {
                GvisSet(idx, chr, stock, 1, "armour rig (helmet on: stock head on)");
                GvisSet(idx, chr, fit, 0, "armour rig (helmet on: hat-fitted head off)");
            } else if (dc) {
                const int mf = engine::object::MeshIndex(dc, SID(fit));
                const int ms = engine::object::MeshIndex(dc, SID(stock));
                if (mf >= 0 && engine::object::MeshHidden(dc, mf))
                    GvisSet(idx, chr, fit, 1, "armour rig (no helmet: hat-fitted head on)");
                if (ms >= 0 && !engine::object::MeshHidden(dc, ms))
                    GvisSet(idx, chr, stock, 0, "armour rig (no helmet: stock head off)");
            }
        }
        if (helm) {             // the helmet must report visible now, else (3 tries) -> rigid attach
            uint8_t* dc = engine::object::DrawControl(chr);
            const int mi = dc ? engine::object::MeshIndex(dc, SID(mesh[0])) : -1;
            const int vis = (mi < 0) ? 1 : (engine::object::MeshHidden(dc, mi) ? 0 : 1);
            if (!vis) {
                if (++r->fail >= 3) {
                    for (int k = 0; k < 2; k++)
                        GvisSet(idx, chr, mesh[k], 0, "armour rig (fail: hide)");
                    if (look_i >= 0)
                        BodyApply(idx, chr, look_i, 0, r->bodyOk);   // back to the original body
                    AR_LOG("armor rig: %s%d - helmet still not visible, rigid attach instead\n",
                        idx == kUs ? "own" : "slot ", idx == kUs ? 0 : idx);
                    return;
                }
            } else
                r->fail = 0;
        }
        if (want != r->shown)
            AR_LOG("armor rig: %s%d helmet %s, vest %s (look %s%s)%s\n",
                idx == kUs ? "own" : "slot ", idx == kUs ? 0 : idx,
                helm ? "SHOWN" : "hidden", (want & 2) ? "SHOWN" : "hidden",
                look_i >= 0 ? kBase[look_i] : "?", kSuffix,
                (clash && helm != wasHelm)
                    ? (helm ? ", clash part hidden" : ", clash part restored") : "");
        r->shown = want;
        r->refresh = engine::frame::Count() + 60;
    }

    int  OwnLook()        { return g_ownLook; }
    int  OwnWant()        { return g_ownWant; }
    bool OwnProbed()      { return g_rig[kUs].probed != 0; }
    int  OwnFail()        { return g_rig[kUs].fail; }
    bool OwnBodyOk(int k) { return (k >= 0 && k < kBodyN) ? g_rig[kUs].bodyOk[k] != 0 : false; }
    const int* OwnBodyOkArr() { return g_rig[kUs].bodyOk; }

    void SetPupBagOff(int idx, bool off)
    {
        if (idx >= 0 && idx < kSlots)
            g_pupOff[idx] = off ? 1 : 0;
    }

    bool PupBagOff(int idx)
    {
        return (idx >= 0 && idx < kSlots) ? g_pupOff[idx] != 0 : false;
    }

    void SetHoodUp(int idx, bool up)
    {
        if (idx >= 0 && idx < kBodies)
            g_hoodUp[idx] = up ? 1 : 0;
    }

    bool HoodUp(int idx)
    {
        return (idx >= 0 && idx < kBodies) ? g_hoodUp[idx] != 0 : false;
    }

    void Reset()
    {
        std::memset(g_rig, 0, sizeof g_rig);
        for (int i = 0; i < kBodies; i++)
            g_rigWk[i] = -1;
        std::memset(g_pupOff, 0, sizeof g_pupOff);
        std::memset(g_hoodUp, 0, sizeof g_hoodUp);
        g_ownWant = -1;
        g_ownLook = -1;
        g_gvisN = 0;
        g_gvisNlog = 0;
    }

}
