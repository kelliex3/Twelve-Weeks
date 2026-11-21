#pragma once

#include <cstdint>

// Ported from tools/ext/melee_duel.c's FAIR WINDOW / dodge-window wiring (the pure part).
// The ext's dodge window narrows to the duel partner's contact (MduWindowEnd), and the
// attacker's next-press gate rises so the victim can dodge out of its hit reaction
// (MduVicExitMs + lead - next contact, capped at base + 60 ms). Both rules are kept here with
// the ext's shipped [TUNING ps*] constants.
//
// This is a PURE core: no engine clips, no psync, no -hit pair, no wire. The host feeds the
// duel swing times (ext vsw_at / vsw_end) and the reaction end / next contact times; the dodge
// core calls WindowEnd through its override seam. With no duel active WindowEnd returns 0.

namespace gameplay::melee_duel {

    inline constexpr int      kMaxPeers        = 8;
    inline constexpr uint16_t kVicDodgeExitMs  = 650;  // ext MDU_VIC_DODGE_EXIT_MS (SP measured)
    inline constexpr uint16_t kVicDodgeLeadMs  = 133;  // ext MDU_VIC_DODGE_LEAD_MS (SP measured)
    inline constexpr uint16_t kFairSlackMs     = 40;   // ext MDU45_FAIR_SLACK_MS [TUNING ps45]
    inline constexpr uint16_t kFairMaxAddMs    = 60;   // ext MDU_FAIR_MAX_MS [TUNING ps24]
    inline constexpr uint16_t kFairCapMs       = 900;  // ext's hard cap on the gate
    inline constexpr uint16_t kNextMs          = 330;  // ext MDU_NEXT_MS (SP)
    inline constexpr uint16_t kNext2Ms         = 500;  // ext MDU_NEXT2_MS
    inline constexpr uint16_t kNext2BreathMs   = 910;  // ext MDU_NEXT2_BREATH_MS
    inline constexpr uint8_t  kVicFreeStep     = 2;    // ext MDU_VIC_FREE_STEP
    inline constexpr float    kReactRate       = 1.12f; // ext MDU_REACT_RATE
    inline constexpr float    kAttLeadMs       = 54.0f; // ext MDG46_LEAD_MS [TUNING ps46]
    inline constexpr float    kAttLeadOldMs    = 90.0f; // ext MD45_LEAD_MS (old toggle)
    inline constexpr float    kOwlCapMs        = 60.0f; // ext MD45_OWL_CAP_MS
    inline constexpr uint16_t kAttFloorMs      = 20;    // ext MD45_ATT_FLOOR_MS
    inline constexpr uint16_t kJitterMs        = 10;    // ext MD45_JITTER_MS
    inline constexpr uint64_t kSwingMatchMs    = 400;   // ext MduWindowEnd's vsw_at vs dodge swing tolerance

    // ext MduVicExitMs: the reaction's exit, or the pair's hand-off when it comes first.
    int VicExitMs(float endSeconds, float reactRate = kReactRate);

    // ext Md45AttLead / Md45VicLead: the lead a dodge press needs on each screen.
    int AttLead(float onewayMs, bool oldLead = false);
    int VicLead(float onewayMs, bool oldLead = false);

    // ext's ps23/ps45 next-press fairness floor. `exitMs` is VicExitMs(), `nextContactMs` the
    // follow-up's authored contact (0 = no follow-up), `baseMs` MDU_NEXT_BASE(), `recoveryMs`
    // the already-earned recovery gate. Returns the effective gate. Old-mode flags mirror the
    // ext's t2r-duel-fair-ps45-old.txt / t2r-duel-fair-uncapped.txt toggles; they default off.
    int FairGateMs(float onewayMs, int exitMs, int nextContactMs, int baseMs, int recoveryMs,
                   bool fair45Old = false, bool uncapped = false);

    // ext MDU_NEXT_BASE() with the shipped tuning (breath / next2-old toggles default off).
    int NextBaseMs(int chainStep, bool breath = false, bool next2Old = false);

    // ---- duel swing window (feeds melee-dodge) ---------------------------------------------
    void SetEnabled(bool enabled) noexcept;   // ext g_mdu.on
    void SetSlot(int slot) noexcept;          // ext g_mdu.slot
    void NoteSwing(int slot, uint64_t atMs, uint64_t endMs); // ext vsw_at[s] / vsw_end[s]
    void EndSwing(int slot);                  // the pair left / was refused: vsw_end[s] = 0
    void EndDuel();                           // MduEnd: no duel window for anyone
    int  WindowEnd(int slot, uint64_t nowMs, uint64_t dodgeSwingMs, uint64_t* end); // 0 / 1 / 2
    bool Active() noexcept;
    void Reset();

} // namespace gameplay::melee_duel
