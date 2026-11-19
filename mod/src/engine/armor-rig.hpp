#pragma once

#include "sid/sid.hpp"

#include <cstdint>

// Ported from tools/ext/t2r-ext.c's skinned-armour section (re/armor-rig.md, re/armor-rattler.md):
// the helmet / vest as LOOK PARTS instead of rigid attachments. Every faction-pool look has an
// armoured twin "<look>-t2r-arm" (the ext's dc_data_patches build adds them: the base look's parts
// + the Rattler helmet slaver-male-helmet-02 + the plate carrier slaver-vest-04-torso, both hidden at
// spawn). The own / puppet spawn swaps the look string for the spawn call only (the wire keeps the
// base name); the two parts -- plus the pushed body / coat copies -- are then shown / hidden with the
// draw-control mesh calls as the armour comes and goes, and the parts the helmet clashes with
// (Seraphite hood up, WLF hats) are hidden while it shows.
//
// This module is the tables + the engine access + the per-body apply. The per-frame DRIVER (ext
// feature_armor_visual, which computes the helmet/vest mask from the armour points and the per-slot
// armour message) is NOT ported: no armour feature exists in the mod yet, so Apply() is never called,
// the own-side rig stays unprobed, and backpack-prop's WornOn / ReapplyRig branch stays inert until
// the producer lands. The two other unported producers (ext g_bpPupOff from backpack.c, g_hoodUp from
// mpsnow.c) have honest setters.
//
// Bodies: 0..kSlots-1 puppet slots, kUs = us (ext T2R_US).

namespace engine::armor_rig {

#ifdef TO1_SLOTS4
    inline constexpr int kSlots = 4;
#else
    inline constexpr int kSlots = 8;
#endif
    inline constexpr int kUs     = kSlots;         // ext T2R_US
    inline constexpr int kBodies = kSlots + 1;

    inline constexpr int kRigN     = 16;           // ext ARMOR_RIG_N
    inline constexpr int kRigMaleN = 12;           // ext ARMOR_RIG_N_MALE (rows >= this are female)
    inline constexpr int kBodyN    = 4;            // ext ARMOR_RIG_BODY_N
    inline constexpr int kFlN      = 8;            // ext FL_N (look slots per faction)

    inline constexpr const char* kSuffix = "-t2r-arm";   // ext ARMOR_RIG_SUFFIX

    // ext ArmorBodyT: the original body part and its pushed / pulled copy, shown / hidden per `mask`
    // (bit 0 helmet, bit 1 vest). copy on (original off) iff the armour piece over it shows.
    struct BodyPair { const char* orig; const char* copy; int mask; };

    // ---- look data -----------------------------------------------------------------------
    const char*  Base(int i);                            // ext ARMOR_RIG_BASES[i] (kRigN rows)
    const char*  FactionLook(int pool, int idx);         // ext FACTION_LOOKS[pool][idx]
    const char*  FactionWinter(int pool, int idx);       // ext FACTION_LOOKS_WINTER[pool][idx]
    const BodyPair* BodyAt(int idx, int look_i, int k);  // ext ArmorBodyAt (winter override applied)

    // ---- decision / twins -----------------------------------------------------------------
    bool On();                                           // ext ArmorRigOn (decides once, then cached)
    int  LookExists(const char* name);                   // ext DcLookExists: 1 yes, 0 no, -1 cannot tell
    int  TwinIdx(uint64_t look);                         // ext ArmorRigTwinIdx (-1 none)
    int  TwinName(const char* look, char* out);          // ext ArmorRigTwinName (out cap 0x28)
    int  SwapField(char* fld, char saved[0x28], const char* who, uint32_t slot);  // ext ArmorRigSwapField
    int  WinterWlfK(uint64_t look);                      // ext ArmorWinterWlfK
    int  WinterTwinRow(uint64_t look);                   // ext ArmorWinterTwinRow
    bool FactionLooksOn();                               // ext !FactionLooksOff (t2r-faction-looks.txt)

    // ---- per-body apply -------------------------------------------------------------------
    bool Failed(int idx, uint64_t key);                  // ext ArmorRigFailed
    // want = piece mask: bit 0 helmet, bit 1 vest. key = a per-character identity (a re-key clears
    // the cached probe / shown state). look_i = the twin row (TwinIdx), -1 = no row.
    void Apply(int idx, uint64_t chr, uint64_t key, int want, int look_i);   // ext ArmorRigApply
    // The pushed / original pair of every body part of look_i (ext ArmorRigBody); `ok` = the cached
    // per-pair presence probe (kBodyN entries).
    void BodyApply(int idx, uint64_t chr, int look_i, int want, const int* ok);

    // ---- own-side state (feature_backpack_prop reads these) --------------------------------
    int  OwnLook();                                      // ext g_rig_own_look
    int  OwnWant();                                      // ext g_rigOwnWant
    bool OwnProbed();                                    // ext g_rig[kUs].probed
    int  OwnFail();                                      // ext g_rig[kUs].fail
    bool OwnBodyOk(int k);                               // ext g_rig[kUs].body_ok[k]
    const int* OwnBodyOkArr();

    // ---- unported producers (honest gates) ------------------------------------------------
    void SetPupBagOff(int idx, bool off);                // ext g_bpPupOff (backpack.c puppet bag-off)
    bool PupBagOff(int idx);
    void SetHoodUp(int idx, bool up);                    // ext g_hoodUp (mpsnow.c winter hood tick)
    bool HoodUp(int idx);

    void Reset();
}
