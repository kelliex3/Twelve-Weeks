// Native owner for tools/ext/customize_compose.c (build 191).
// See customize-compose-core.hpp for the pure cache / selection rules.

#include "customize-compose.hpp"
#include "customize-compose-core.hpp"

#include "boot.hpp"
#include "customize.hpp"
#include "engine/armor-rig.hpp"
#include "engine/backpack-prop.hpp"
#include "engine/exe_side.hpp"
#include "engine/ptr.hpp"
#include "log.hpp"
#include "net/match/net-player.hpp"
#include "sid/sid.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace net::ui::customize::compose {

    namespace {

        using namespace core;

        struct Snap {
            uint64_t* arr = nullptr;
            uint32_t  n = 0;
            uint64_t  m[kSupMax]{};
            uint64_t* parr = nullptr;
            uint32_t  pn = 0;
            uint64_t  p[kPSupMax]{};
        };

        Snap      g_snap[CZ_NFAC][CZ_NSEX][kMaxBodies];
        Body      g_body[kMaxBodies];
        WireCache g_wire[kMaxPeers];
        bool      g_off = false;
        uint64_t  g_nextTry = 0;

        void CurrentSlotName(int slot, char out[24])
        {
            out[0] = 0;
            if (slot < 0 || slot >= kMaxPeers) return;
            const char* n = NetPlayers::BySlot(slot).name;
            if (!n) return;
            std::snprintf(out, 24, "%s", n);
        }

        int FacOfLook(const char* look)
        {
            if (!look) return -1;
            if (std::strncmp(look, "scar-", 5) == 0) return 1;
            if (std::strncmp(look, "militia-", 8) == 0) return 0;
            return -1;
        }

        int SexOfLook(const char* look)
        {
            if (!look) return -1;
            if (std::strstr(look, "female")) return 1;
            if (std::strstr(look, "male")) return 0;
            return -1;
        }

        const char* MeshName(uint64_t m)
        {
            if (!m || !engine::ptr::Readable(static_cast<uintptr_t>(m), 0x50)) return nullptr;
            const char* s = *reinterpret_cast<const char* const*>(m + 0x10);
            return s && engine::ptr::Readable(reinterpret_cast<uintptr_t>(s), 8) ? s : nullptr;
        }

        bool MakeWritable(void* p, size_t bytes)
        {
            if (!p) return false;
            if (engine::ptr::Writable(reinterpret_cast<uintptr_t>(p), bytes)) return true;
            DWORD old = 0;
            return VirtualProtect(p, bytes, PAGE_READWRITE, &old) != 0;
        }

        bool RecNames(uint8_t* rec, const char** out, int cap)
        {
            if (!out || cap <= 0) return false;
            int k = 0;
            uint64_t* arr = rec ? *reinterpret_cast<uint64_t**>(rec + 0xa0) : nullptr;
            const uint32_t n = rec ? *reinterpret_cast<uint32_t*>(rec + 0xa8) : 0;
            if (arr && n <= kSupMax
                && engine::ptr::Readable(reinterpret_cast<uintptr_t>(arr), n * sizeof(uint64_t))) {
                for (uint32_t i = 0; i < n && k < cap - 1; ++i) {
                    const char* s = MeshName(arr[i]);
                    if (s) out[k++] = s;
                }
            }
            out[k] = nullptr;
            return k > 0;
        }

        bool RecHas(uint8_t* rec, const char* part)
        {
            if (!rec || !part) return false;
            uint64_t* arr = *reinterpret_cast<uint64_t**>(rec + 0xa0);
            const uint32_t n = *reinterpret_cast<uint32_t*>(rec + 0xa8);
            if (!arr || n > kSupMax
                || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(arr), n * sizeof(uint64_t)))
                return false;
            for (uint32_t i = 0; i < n; ++i) {
                const char* s = MeshName(arr[i]);
                if (s && std::strcmp(s, part) == 0) return true;
            }
            return false;
        }

        uint8_t* TwinRec(int row)
        {
            const char* base = engine::armor_rig::Base(row);
            if (!base) return nullptr;
            char name[0x40];
            std::snprintf(name, sizeof name, "%s%s", base, engine::armor_rig::kSuffix);
            return engine::armor_rig::LookRecord(name);
        }

        int MatchRow(int fac, int sex, int cat, int idx)
        {
            int rows[8];
            const int n = PoolRows(fac, sex, rows);
            if (n <= 0) return -1;
            const cz_item_t* it = Item(fac, sex, cat, idx);
            const char* p0 = it && it->parts ? it->parts[0] : nullptr;
            for (int i = 0; p0 && i < n; ++i)
                if (RecHas(TwinRec(rows[i]), p0)) return rows[i];
            return rows[0];
        }

        // ext cz_tint(): select the face's static palette, or copy the matching
        // face entry into the record's own palette when the face records are not
        // in this build.
        void ApplyTint(uint8_t* rec, uint64_t ownSid, const char* head,
                       int faceIdx, int nface)
        {
            char fl[0x40];
            if (!head || !FaceLook(head, fl, sizeof fl)) return;
            const uint32_t pc = *reinterpret_cast<uint32_t*>(rec + 0xe8);
            if (pc < 1) return;
            uint64_t* pa = *reinterpret_cast<uint64_t**>(rec + 0xe0);
            if (!pa || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(pa), 8)
                || !pa[0] || !engine::ptr::Readable(static_cast<uintptr_t>(pa[0]), 16))
                return;

            int slot = -1;
            if (PaletteSlot(faceIdx, nface, slot)
                && engine::ptr::Readable(reinterpret_cast<uintptr_t>(pa),
                                         static_cast<size_t>(nface + 2) * sizeof(uint64_t))) {
                const uint64_t tag = SID("look2-tint-palette");
                bool ok = true;
                for (int i = 1; i < nface + 2 && ok; ++i)
                    ok = pa[i] != 0
                      && engine::ptr::Readable(static_cast<uintptr_t>(pa[i]) - 8, 0x18)
                      && *reinterpret_cast<uint64_t*>(pa[i] - 8) == tag;
                if (ok && MakeWritable(pa, sizeof(uint64_t))) {
                    if (pa[0] != pa[slot])
                        TO1_LOG("customize compose: skin tint palette of %s (face %d of %d)\n",
                                fl, faceIdx, nface);
                    pa[0] = pa[slot];
                    return;
                }
            }

            uint8_t* pal = reinterpret_cast<uint8_t*>(pa[0]);
            uint64_t* ents = *reinterpret_cast<uint64_t**>(pal);
            const uint32_t n = *reinterpret_cast<uint32_t*>(pal + 8);
            if (!ents || n > 256
                || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(ents),
                                          n * sizeof(uint64_t)))
                return;
            const uint64_t want = SID(fl);
            uint8_t* own = nullptr, *src = nullptr;
            for (uint32_t i = 0; i < n; ++i) {
                uint8_t* e = reinterpret_cast<uint8_t*>(ents[i]);
                if (!e || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(e), 0x18)) continue;
                const uint64_t sid = *reinterpret_cast<uint64_t*>(e);
                if (sid == ownSid) own = e;
                if (sid == want) src = e;
            }
            if (own && src && MakeWritable(own + 8, 0x10))
                std::memcpy(own + 8, src + 8, 0x10);
            else
                TO1_LOG("customize compose: no skin tint for %s\n", head);
        }

        void ClearBody(int body)
        {
            if (body < 0 || body >= kMaxBodies) return;
            g_body[body] = Body{};
            engine::backpack_prop::ClearComposedBagLook(body);
        }

        bool RefreshOff(uint64_t now)
        {
            if (now >= g_nextTry) {
                g_nextTry = now + 2000;
                const bool off = engine::exe_side::Exists("t2r-no-customize-look.txt");
                if (off != g_off) {
                    g_off = off;
                    TO1_LOG("customize compose: %s\n", off ? "OFF (pool looks)" : "on");
                }
            }
            return g_off;
        }

    } // namespace

    void NotePeer(int slot, const uint8_t* data, uint8_t len, const char* name)
    {
        if (boot::ExtLoaded() || slot < 0 || slot >= kMaxPeers || !data) return;
        Wire w;
        if (Recv(data, len, w) == 0) {
            TO1_LOG("customize compose: slot %d outfit rejected (len %u)\n", slot, len);
            return;
        }
        char nm[24] = {};
        if (name) std::snprintf(nm, sizeof nm, "%s", name);
        const bool changed = WireChanged(g_wire[slot], w, nm);
        g_wire[slot].wire = w;
        g_wire[slot].at = GetTickCount64();
        std::memcpy(g_wire[slot].name, nm, sizeof nm);
        if (changed)
            TO1_LOG("customize compose: slot %d outfit WLF m/f %u/%u, SERAPHITES %u/%u"
                    " (applies at its next spawn)\n", slot,
                    w.it[0][CZ_TORSO], w.it[0][CZ_LOWER],
                    w.it[1][CZ_TORSO], w.it[1][CZ_LOWER]);
    }

    void Tick(uint64_t now)
    {
        if (boot::ExtLoaded()) return;
        RefreshOff(now);
        for (int i = 0; i < kMaxPeers; ++i) {
            if (!g_wire[i].wire.ok) continue;
            char nm[24];
            CurrentSlotName(i, nm);
            if (!WireCurrent(g_wire[i], nm)) {
                g_wire[i] = WireCache{};
                ClearBody(i);
            }
        }
    }

    void Reset()
    {
        for (auto& s : g_snap) for (auto& t : s) for (auto& v : t) v = Snap{};
        for (auto& b : g_body) b = Body{};
        for (auto& w : g_wire) w = WireCache{};
        g_off = false;
        g_nextTry = 0;
        for (int i = 0; i < kMaxBodies; ++i)
            engine::backpack_prop::ClearComposedBagLook(i);
    }

    bool WearOff()
    {
        return boot::ExtLoaded() || RefreshOff(::GetTickCount64());
    }

    namespace {

        // ext's cz_no_pushed_props: the CUSTOMIZE stage composes a look for a
        // separate NPC and must not pull the local player's pushed props into it.
        struct PushedPropsGuard {
            int prev;
            explicit PushedPropsGuard(bool suppress)
                : prev(core::cz_no_pushed_props)
            {
                core::cz_no_pushed_props = suppress ? 1 : 0;
            }
            ~PushedPropsGuard() { core::cz_no_pushed_props = prev; }
        };

        bool ComposeLook(int body, int fac, int sex, const uint8_t it[CZ_NCAT],
                         bool suppressPushed, char out[kNameLen])
        {
            if (out) out[0] = 0;
            if (boot::ExtLoaded() || !engine::armor_rig::FactionLooksOn()
                || !out || !it || body < 0 || body >= kMaxBodies
                || fac < 0 || fac >= CZ_NFAC || sex < 0 || sex >= CZ_NSEX)
                return false;
            PushedPropsGuard pushed(suppressPushed);

            char name[kNameLen];
            if (!LookName(fac, sex, body, name, sizeof name)) { ClearBody(body); return false; }
            uint8_t* rec = engine::armor_rig::LookRecord(name);
            if (!rec) { ClearBody(body); return false; }

            // Face records share their mesh / prop arrays with the body record but
            // carry the face's skin colour in their palette.
            bool skinRec = false;
            char faceName[kNameLen];
            const int faceRecord = FaceRecordIndex(fac, sex, it[CZ_FACE]);
            if (FaceRecordName(name, faceRecord, faceName, sizeof faceName)) {
                uint8_t* vp = engine::armor_rig::LookRecord(faceName);
                if (vp && *reinterpret_cast<uint64_t**>(vp + 0xa0)
                       == *reinterpret_cast<uint64_t**>(rec + 0xa0)) {
                    rec = vp;
                    std::memcpy(name, faceName, std::strlen(faceName) + 1);
                    skinRec = true;
                }
            }

            Snap& s = g_snap[fac][sex][body];
            uint64_t* arr = *reinterpret_cast<uint64_t**>(rec + 0xa0);
            uint64_t* parr = rec ? *reinterpret_cast<uint64_t**>(rec + 0xb0) : nullptr;
            const uint32_t n = *reinterpret_cast<uint32_t*>(rec + 0xa8);
            const uint32_t pn = parr ? *reinterpret_cast<uint32_t*>(rec + 0xb8) : 0;
            if (!arr || n > kSupMax || pn > kPSupMax
                || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(arr),
                                          n * sizeof(uint64_t))
                || (pn && !engine::ptr::Readable(reinterpret_cast<uintptr_t>(parr),
                                                 pn * sizeof(uint64_t)))) {
                TO1_LOG("customize compose: %s superset unreadable (%u parts, %u props)\n",
                        name, n, pn);
                ClearBody(body);
                return false;
            }
            if (s.arr != arr) {
                std::memcpy(s.m, arr, n * sizeof(uint64_t));
                s.n = n;
                s.arr = arr;
                if (pn) std::memcpy(s.p, parr, pn * sizeof(uint64_t));
                s.pn = pn;
                s.parr = parr;
            }

            Part sup[kSupMax];
            uint8_t keep[kSupMax], pkeep[kPSupMax]{};
            for (uint32_t i = 0; i < s.n; ++i) {
                const char* nm = MeshName(s.m[i]);
                sup[i].name = nm ? nm : "";
                sup[i].hidden = nm
                    ? (*reinterpret_cast<const uint8_t*>(s.m[i] + 0x48) != 0) : 1;
            }
            const int rt = MatchRow(fac, sex, CZ_TORSO, it[CZ_TORSO]);
            const int rl = MatchRow(fac, sex, CZ_LOWER, it[CZ_LOWER]);
            const int rb = MatchRow(fac, sex, CZ_BACKPACK, it[CZ_BACKPACK]);
            const char* tx[48] = {};
            const char* lx[48] = {};
            RecNames(TwinRec(rt), tx, 48);
            RecNames(TwinRec(rl), lx, 48);
            const int kept = Select(fac, sex, it, sup, static_cast<int>(s.n), tx, lx, keep);
            if (kept <= 0) { ClearBody(body); return false; }
            const char* pnm[kPSupMax];
            for (uint32_t i = 0; i < s.pn; ++i) {
                const char* nm = MeshName(s.p[i]);
                pnm[i] = nm ? nm : "";
            }
            const int keptProps = s.pn
                ? SelectProps(fac, sex, it, pnm, static_cast<int>(s.pn), pkeep) : 0;

            if (!MakeWritable(arr, s.n * sizeof(uint64_t))
                || (s.pn && !MakeWritable(parr, s.pn * sizeof(uint64_t)))
                || !MakeWritable(reinterpret_cast<uint8_t*>(rec) + 0xa8, 0x14)) {
                TO1_LOG("customize compose: %s arrays not writable\n", name);
                ClearBody(body);
                return false;
            }
            uint32_t j = 0;
            for (uint32_t i = 0; i < s.n; ++i) if (keep[i]) arr[j++] = s.m[i];
            for (uint32_t i = j; i < s.n; ++i) arr[i] = s.m[i];
            *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(rec) + 0xa8) = j;
            if (s.pn) {
                uint32_t q = 0;
                for (uint32_t i = 0; i < s.pn; ++i) if (pkeep[i]) parr[q++] = s.p[i];
                for (uint32_t i = 0; i < s.pn; ++i) if (!pkeep[i]) parr[q++] = s.p[i];
                *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(rec) + 0xb8) =
                    static_cast<uint32_t>(keptProps);
            }

            const cz_item_t* face = Item(fac, sex, CZ_FACE, it[CZ_FACE]);
            if (!skinRec)
                ApplyTint(rec, SID(name), face && face->parts ? face->parts[0] : nullptr,
                          it[CZ_FACE], Count(fac, sex, CZ_FACE));

            Body& B = g_body[body];
            B = Body{};
            B.on = true;
            B.fac = fac;
            B.sex = sex;
            B.rowTorso = rt;
            B.rowLower = rl;
            B.rowBag = rb;
            B.sid = SID(name);
            std::snprintf(B.name, sizeof B.name, "%s", name);
            B.voice = FaceVoice(fac, sex, it[CZ_FACE]);

            const char* orig[kPairMax] = {};
            const char* copy[kPairMax] = {};
            int mask[kPairMax] = {};
            B.npair = ArmourPairs(sup, keep, static_cast<int>(s.n),
                                  orig, copy, mask, kPairMax);
            for (int i = 0; i < B.npair; ++i) {
                std::snprintf(B.pairOrig[i], sizeof B.pairOrig[i], "%s", orig[i]);
                std::snprintf(B.pairCopy[i], sizeof B.pairCopy[i], "%s", copy[i]);
                B.pairMask[i] = mask[i];
            }
            const cz_item_t* hat = Item(fac, sex, CZ_HAT, it[CZ_HAT]);
            if (fac == 1)
                std::snprintf(B.clash, sizeof B.clash, "scar-%s-hoodup",
                              sex ? "female" : "male");
            else if (hat && hat->parts && hat->parts[0])
                std::snprintf(B.clash, sizeof B.clash, "%s", hat->parts[0]);
            B.nhair = HairPlan(fac, sex, it[CZ_FACE], sup, keep, static_cast<int>(s.n),
                               B.pairOrig, B.npair, B.hair, B.headFit, B.headStock);

            engine::backpack_prop::SetComposedBagLook(body, B.sid, rb);
            std::memcpy(out, name, std::strlen(name) + 1);
            TO1_LOG("customize compose: body %d -> %s (%u of %u parts, %d props, %d pairs)\n",
                    body, name, j, s.n, keptProps, B.npair);
            return true;
        }

    } // namespace

    bool SpawnLook(int body, const char* poolLook, char out[0x28])
    {
        if (out) out[0] = 0;
        if (boot::ExtLoaded() || WearOff() || !engine::armor_rig::FactionLooksOn()
            || !out || !poolLook || !poolLook[0]
            || body < 0 || body >= kMaxBodies)
            return false;

        const int fac = FacOfLook(poolLook), sex = SexOfLook(poolLook);
        if (fac < 0 || sex < 0) { ClearBody(body); return false; }
        const bool isUs = body == engine::armor_rig::kUs;
        bool peerValid = false;
        if (!isUs && body < kMaxPeers) {
            char nm[24];
            CurrentSlotName(body, nm);
            peerValid = WireCurrent(g_wire[body], nm);
            if (!peerValid) {
                g_wire[body] = WireCache{};
                ClearBody(body);
            }
        }
        uint8_t it[CZ_NCAT];
        if (!OutfitFor(customize::GetProfile(), g_wire[body < kMaxPeers ? body : 0],
                       isUs, peerValid, fac, sex, it)) {
            ClearBody(body);
            return false;
        }
        return ComposeLook(body, fac, sex, it, false, out);
    }

    bool SpawnLookItems(int body, int fac, int sex, const uint8_t it[CZ_NCAT],
                        char out[0x28])
    {
        return ComposeLook(body, fac, sex, it, true, out);
    }

    int RowOfLook(uint64_t look, int which)
    {
        if (!look) return -1;
        for (int i = 0; i < kMaxBodies; ++i)
            if (g_body[i].on && g_body[i].sid == look) return RowOfBody(g_body[i], which);
        return -1;
    }

    bool HasComposedBody(int body)
    {
        return body >= 0 && body < kMaxBodies && g_body[body].on;
    }

    const char* BodyPairOrig(int body, int k)
    {
        if (body < 0 || body >= kMaxBodies || !g_body[body].on
            || k < 0 || k >= g_body[body].npair) return nullptr;
        return g_body[body].pairOrig[k];
    }

    const char* BodyPairCopy(int body, int k)
    {
        if (body < 0 || body >= kMaxBodies || !g_body[body].on
            || k < 0 || k >= g_body[body].npair) return nullptr;
        return g_body[body].pairCopy[k];
    }

    int BodyPairMask(int body, int k)
    {
        if (body < 0 || body >= kMaxBodies || !g_body[body].on
            || k < 0 || k >= g_body[body].npair) return 0;
        return g_body[body].pairMask[k];
    }

    const char* BodyClash(int body, const char* fallback)
    {
        // A composed body with no clash string deliberately suppresses the
        // pool look's fallback (ext cz_body_clash).
        if (body >= 0 && body < kMaxBodies && g_body[body].on)
            return g_body[body].clash[0] ? g_body[body].clash : nullptr;
        return fallback;
    }

    bool BodyHeadSwap(int body, const char** fit, const char** stock)
    {
        if (!fit || !stock || body < 0 || body >= kMaxBodies
            || !g_body[body].on || !g_body[body].headFit[0]) return false;
        *fit = g_body[body].headFit;
        *stock = g_body[body].headStock;
        return true;
    }

    const char* BodyHair(int body, int k)
    {
        if (body < 0 || body >= kMaxBodies || !g_body[body].on
            || k < 0 || k >= g_body[body].nhair) return nullptr;
        return g_body[body].hair[k];
    }

    int BodyVoice(int body, int voxFac)
    {
        if (body < 0 || body >= kMaxBodies || !g_body[body].on
            || (voxFac != 0 && voxFac != 1) || g_body[body].fac != !voxFac) return -1;
        return g_body[body].voice;
    }

} // namespace net::ui::customize::compose
