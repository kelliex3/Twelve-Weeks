#pragma once

#include "sid/sid.hpp"

#include <cstdint>

class NetPlayer;

namespace gameplay::melee {

    namespace tuning {

        inline constexpr bool  kLockOnEnabled = true;
        inline constexpr float kLockOnDistanceM = 2.6f;
        inline constexpr float kSprintLockOnDistanceM = 5.5f;
        inline constexpr float kLockOnLeadSec = 0.15f;
        inline constexpr float kLockOnLeadMaxM = 1.5f;
        inline constexpr float kLockOnMaxHeightDeltaM = 1.2f;
        inline constexpr float kLockOnMinDistanceM = 0.25f;

        inline constexpr float kMeleeDamageOverride = 20.0f; // 0 = default 15 damage.
        inline constexpr float kMeleeDamageFallback = 35.0f;
        inline constexpr float kMeleeDamageMax = 100.0f;
        inline constexpr uint32_t kAttackKindMelee = 1;
        inline constexpr bool  kSuppressLocalPuppetMeleeDamage = true;
        inline constexpr bool  kSuppressReplayedSwingDamage = true;

        inline constexpr bool  kSubstituteMeleeTarget = true;
        inline constexpr float kSubstituteRadiusM = 3.0f;
        inline constexpr float kSprintSubstituteRadiusM = 3.5f;
        inline constexpr float kSubstituteConeHalfAngleDeg = 120.0f;
        inline constexpr float kSubstituteMaxHeightDeltaM = 1.5f;

        inline constexpr float kMeleeEventRadiusScale = 1.25f;
        inline constexpr float kMeleeEventHeightScale = 1.15f;
        inline constexpr uint32_t kMeleeEventDataOffset = 0x20;
        inline constexpr uint32_t kMeleeEventRadiusOffset = 0x10;
        inline constexpr uint32_t kMeleeEventHeightOffset = 0x14;

        inline constexpr uint32_t kRvaDamageTargetInRadius = 0x0EDA170;

        inline constexpr bool       kExecutionEnabled = true;
        inline constexpr StringId64 kExecutionMove = SID("fist-throat-punch-kill");
        inline constexpr float      kExecutionMaxDistanceM = 2.5f;
        inline constexpr float      kExecutionCooldownSec = 3.0f;
        inline constexpr float      kExecutionAckTimeoutSec = 1.0f;
        inline constexpr float      kExecutionPromiseTimeoutSec = 2.0f;
        inline constexpr float      kExecutionSyncGiveUpSec = 0.75f;
        inline constexpr float      kExecutionMinHoldSec = 0.5f;
        inline constexpr float      kExecutionMaxHoldSec = 4.0f;

        inline constexpr float      kMeleeHitSoundHeightM = 1.2f;
        inline constexpr StringId64 kMeleeHitSfx[] = {
            SID("sfx-unarmed-hit-face-skin"),
            SID("sfx-unarmed-hit-gut-cotton"),
            SID("sfx-unarmed-hit-head-skin"),
            SID("sfx-unarmed-hit-gut"),
        };
        inline constexpr StringId64 kMeleeLethalHitSfx[] = {
            SID("sfx-unarmed-finisher-face-skin"),
            SID("sfx-unarmed-breaker-face-skin"),
            SID("sfx-unarmed-finisher-gut-skin"),
            SID("sfx-unarmed-finisher-face-chin"),
        };
    }

    bool      LockOnAttack(uintptr_t player, float* dir, float* stick, float* mag);

    uintptr_t CurrentMeleeActionOf(uintptr_t player);
    uintptr_t SubstituteMeleeTarget(uintptr_t player);

    float     ResolveMeleeDamage(float engineDamage);
    void      PlayLocalHitFeedback(NetPlayer& victim, bool lethal);
    inline constexpr int kExecReplyRefused = 0;
    inline constexpr int kExecReplyAccepted = 1;
    inline constexpr int kExecReplyRefusedMutual = 2;
    inline constexpr int kExecReplyRefusedClaimed = 3;

    bool      QueueExecution(const NetPlayer& victim, uint64_t weapon, float damage);
    int       AcceptIncomingExecution(int attackerSlot);
    void      OnExecutionReply(int victimSlot, int replyCode);
    void      OnExecutedBy(int attackerSlot, StringId64 attackerNpcSid, StringId64 moveSid);
    int       ExecutionTargetSlot();
    bool      PuppetFrozenForExecution(int slot);
    bool      BeingExecuted();
    bool      CommittedToExecutor();
    bool      BeingExecutedBy(int attackerSlot);
    void      ExemptMeleeTarget(StringId64 npcSid, float seconds);
    bool      MeleeTargetExempt(StringId64 npcSid);

    void      Tick(float dt);
    void      Reset();
    void      NotifyPuppetGone(int slot);
}
