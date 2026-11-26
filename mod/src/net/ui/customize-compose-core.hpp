#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Pure planning/cache layer for tools/ext/customize_compose.c (build 191).
// The native owner in customize-compose.cpp owns look2 record access, file
// reads, widget/input seams and all engine calls; nothing here touches them.
// Wire stays version 2 / 18 bytes / NEVT_CUSTOM_LOOK 0x59 (customize-core.hpp).

#include "customize-core.hpp"

namespace net::ui::customize::compose::core {

    using namespace net::ui::customize::core;

    inline constexpr int kMaxBodies   = 16;      // ext CZB_N with margin (T2R_MAX_PEERS + 1)
    inline constexpr int kMaxPeers    = 16;
    inline constexpr int kSupMax      = 512;     // ext CZ_SUP_MAX
    inline constexpr int kPSupMax     = 16;      // ext CZ_PSUP_MAX
    inline constexpr int kNameLen     = 0x28;    // look2 look-name field
    inline constexpr int kPairMax     = 4;       // ARMOR_RIG_BODY_N
    inline constexpr int kHairMax     = 4;       // CZ_HAIR_CLASH_N
    inline constexpr int kPartNameLen = 0x30;

    // ext cz_body_t / cz_wire_t state, kept as plain data so the owner can be
    // exercised with a fake look2 record in a standalone test.
    struct Body {
        bool     on = false;
        int      fac = -1, sex = -1;
        int      rowTorso = -1, rowLower = -1, rowBag = -1;
        uint64_t sid = 0;
        char     name[kNameLen]{};
        int      npair = 0;
        char     pairOrig[kPairMax][kPartNameLen]{};
        char     pairCopy[kPairMax][kPartNameLen]{};
        int      pairMask[kPairMax]{};
        int      nhair = 0;
        char     hair[kHairMax][kPartNameLen]{};
        char     clash[kPartNameLen]{};
        char     headFit[kPartNameLen]{};
        char     headStock[kPartNameLen]{};
        int      voice = -1;
    };

    struct WireCache {
        Wire     wire{};
        uint64_t at = 0;
        char     name[24]{};
    };

    // ext cz_pool_rows(): ARMOR_RIG_BASES rows of one (faction, sex).
    inline int PoolRows(int fac, int sex, int out[8]) noexcept
    {
        if (!out || (fac != 0 && fac != 1) || (sex != 0 && sex != 1)) return 0;
        int n = 0;
        if (sex == 0) {
            const int b = fac ? 0 : 6;
            for (int i = b; i < b + 6 && n < 8; ++i) out[n++] = i;
        } else if (sex == 1) {
            const int b = fac ? 12 : 14;
            for (int i = b; i < b + 2 && n < 8; ++i) out[n++] = i;
        }
        return n;
    }

    inline bool WireCurrent(const WireCache& c, const char* name) noexcept
    {
        // ext cz_wire_current(): an empty name from either side means "keep".
        return !name || !name[0] || !c.name[0] || std::strcmp(c.name, name) == 0;
    }

    inline bool WireChanged(const WireCache& c, const Wire& w,
                            const char* name) noexcept
    {
        return !c.wire.ok
            || std::memcmp(&c.wire.sex, &w.sex, sizeof w.sex) != 0
            || std::memcmp(c.wire.it, w.it, sizeof w.it) != 0
            || std::strcmp(c.name, name ? name : "") != 0;
    }

    // ext cz_outfit(): our own body uses the profile; a puppet uses the wire of
    // its slot, with defaults when the wire carries the other sex's items.
    inline bool OutfitFor(const Profile& p, const WireCache& c, bool isUs,
                          bool peerValid, int fac, int sex, uint8_t out[]) noexcept
    {
        if (!out || fac < 0 || fac >= CZ_NFAC || sex < 0 || sex >= CZ_NSEX) return false;
        if (isUs) {
            for (int i = 0; i < kWearCats; ++i) out[i] = p.it[fac][sex][i];
            return true;
        }
        if (!peerValid || !c.wire.ok || !WireCurrent(c, c.name)) return false;
        if (sex != c.wire.sex[fac]) {
            for (int i = 0; i < kWearCats; ++i)
                out[i] = static_cast<uint8_t>(CZ_DEFAULT[fac][sex][i]);
            return true;
        }
        std::memcpy(out, c.wire.it[fac], kWearCats);
        return true;
    }

    // ext customize_looks.face_name: "<body record>-<face index>".
    // WLF skin-tone group per face (customize_items.h CZ_WLF_TONE). Ext
    // customize_compose.c maps the face index before looking up its record.
    inline int FaceRecordIndex(int fac, int sex, int faceIdx) noexcept
    {
        if (fac != 0 || sex < 0 || sex >= CZ_NSEX) return faceIdx;
        if (faceIdx < 0) return 0;
        return faceIdx < CZ_WLF_TONES
             ? static_cast<int>(CZ_WLF_TONE[sex][faceIdx]) : 0;
    }

    inline bool FaceRecordName(const char* bodyName, int faceIdx,
                               char* out, int cap) noexcept
    {
        if (!bodyName || !out || cap <= 0 || faceIdx < 0) return false;
        const int n = std::snprintf(out, static_cast<size_t>(cap), "%s-%d",
                                    bodyName, faceIdx);
        return n > 0 && n < cap;
    }

    // ext cz_tint()'s palette slot: pa[0] is the live palette, pa[1] the
    // default, pa[2 + face] the per-face skin. Validation of the pointers
    // themselves stays in the owner.
    inline bool PaletteSlot(int faceIdx, int nface, int& index) noexcept
    {
        if (faceIdx < 0 || nface <= 0 || faceIdx >= nface) return false;
        index = 2 + faceIdx;
        return true;
    }

    // ext cz_face_voice(): KELLIE keeps female voice C in every outfit.
    inline int FaceVoice(int fac, int sex, int faceIdx) noexcept
    {
        (void)fac;
        if (sex != 1) return -1;
        const cz_item_t* face = Item(fac, sex, CZ_FACE, faceIdx);
        if (!face || !face->parts || !face->parts[0]) return -1;
        const char* head = face->parts[0];
        return std::strcmp(head, "t2r-kl-f-head-kellie01") == 0
            || std::strcmp(head, "t2r-kl-s-head-kellie01") == 0 ? 2 : -1;
    }

    // ext customize_compose.c:258-287. Only hair parts that are actually drawn
    // are listed; a hat-fitted second head swaps with the stock head instead.
    inline int HairPlan(int fac, int sex, int faceIdx, const Part* sup,
                        const uint8_t* keep, int n,
                        const char pairOrig[kPairMax][kPartNameLen], int npair,
                        char hair[kHairMax][kPartNameLen],
                        char headFit[kPartNameLen],
                        char headStock[kPartNameLen]) noexcept
    {
        if (hair) for (int i = 0; i < kHairMax; ++i) hair[i][0] = 0;
        if (headFit) headFit[0] = 0;
        if (headStock) headStock[0] = 0;
        const cz_item_t* face = Item(fac, sex, CZ_FACE, faceIdx);
        if (!face || !face->parts || !face->parts[0] || !sup || !keep) return 0;
        const char* stock = face->parts[0];
        bool stockHidden = false, stockPaired = false;
        for (int i = 0; stock && i < n; ++i)
            if (keep[i] && std::strcmp(sup[i].name, stock) == 0)
                stockHidden = sup[i].hidden != 0;
        for (int i = 0; stock && i < npair; ++i)
            if (std::strcmp(pairOrig[i], stock) == 0) stockPaired = true;
        int count = 0;
        for (const char* const* fp = face->parts; *fp && count < kHairMax; ++fp) {
            const bool first = fp == face->parts;
            const bool perHat = !first && std::strstr(*fp, "-head-") == nullptr
                && (std::strncmp(*fp, "t2r-fh-", 7) == 0
                    || (std::strncmp(*fp, "t2r-kl-", 7) == 0 && std::strstr(*fp, "-h-")));
            const bool hairPart = std::strstr(*fp, "-hair") != nullptr
                && std::strstr(*fp, "-head-") == nullptr;
            const bool secondHead = !first && std::strstr(*fp, "-head-") != nullptr;
            if (!hairPart && !perHat && !secondHead) continue;
            bool drawn = false;
            for (int i = 0; i < n && !drawn; ++i)
                drawn = keep[i] && !sup[i].hidden && std::strcmp(sup[i].name, *fp) == 0;
            if (!drawn) continue;
            if (secondHead && stockHidden && !stockPaired && headFit && headFit[0] == 0) {
                std::snprintf(headFit, kPartNameLen, "%s", *fp);
                std::snprintf(headStock, kPartNameLen, "%s", stock);
                continue;
            }
            std::snprintf(hair[count++], kPartNameLen, "%s", *fp);
        }
        return count;
    }

    inline int RowOfBody(const Body& b, int which) noexcept
    {
        if (!b.on) return -1;
        return which == 0 ? b.rowTorso : which == 1 ? b.rowLower
             : which == 2 ? b.rowBag : -1;
    }

} // namespace net::ui::customize::compose::core
