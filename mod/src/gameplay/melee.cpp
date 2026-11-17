#include "melee.hpp"
#include "engine/script.hpp"
#include "net-player.hpp"
#include "net-session.hpp"
#include "net-hooks.hpp"
#include "net-engine.hpp"
#include "anim.hpp"
#include "log.hpp"

#include <algorithm>
#include <cmath>

#undef min
#undef max

namespace gameplay::melee {

    using namespace tuning;

    namespace {

        constexpr float kPi = 3.14159265358979323846f;
        constexpr float kDegToRad = kPi / 180.0f;
        constexpr float kRadToDeg = 180.0f / kPi;

        struct Candidate {
            int   slot = -1;
            float dist = 0.0f;
            float dx = 0.0f;
            float dz = 0.0f;
        };

        struct SwingState {
            bool  sprint = false;
            float speed = 0.0f;
        };

        struct Execution {
            enum class Phase { Idle, Requested, Running };
            Phase      phase = Phase::Idle;
            StringId64 npcSid = 0;
            int        slot = -1;
            float      age = 0.0f;
            uint64_t   weapon = 0;
            float      damage = 0.0f;
            bool       sawMelee = false;
            bool       syncSent = false;
            bool       forfeitKill = false;
        };

        struct VictimExecution {
            bool       active = false;
            int        slot = -1;
            StringId64 npcSid = 0;
            float      age = 0.0f;
            StringId64 moveSid = 0;
            uintptr_t  playerPtr = 0;
            bool       sawPlayerMelee = false;   // our half has actually begun
            bool       sawNpcMelee = false;      // the attacker's half has actually begun
        };

        SwingState s_swing;
        Execution s_execution;
        VictimExecution s_executedBy;

        int   s_promisedTo = -1;
        float s_promisedAge = 0.0f;
        StringId64 s_exemptSid = 0;
        float      s_exemptTtl = 0.0f;
        float s_executionCooldown = 0.0f;
        uint64_t s_lastLoggedMove = 0;
        uint64_t s_lastLoggedDefMove = 0;
        bool     s_lastLoggedDead = false;

        float WrapRad(float a)
        {
            while (a > kPi)  a -= 2.0f * kPi;
            while (a < -kPi) a += 2.0f * kPi;
            return a;
        }

        float YawOf(const Quaternion& q)
        {
            const Vector3 fwd = q.rotate(Vector3(0.0f, 0.0f, 1.0f));
            return std::atan2(fwd.x, fwd.z);
        }

        Quaternion YawQuat(float rad)
        {
            const float h = rad * 0.5f;
            return Quaternion(0.0f, std::sin(h), 0.0f, std::cos(h));
        }

        Quaternion Normalized(const Quaternion& q)
        {
            const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            if (!(len > 1e-6f)) return Quaternion::Identity();
            return Quaternion(q.x / len, q.y / len, q.z / len, q.w / len);
        }

        bool Finite(const Vector3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        bool Finite(const Quaternion& q)
        {
            return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
        }

        bool PuppetTargetable(const NetPlayer& p, const char** why)
        {
            *why = nullptr;
            if (!p.IsActiveRemote()) { *why = "inactive"; return false; }
            if (!p.spawned || !p.npcSid) { *why = "not spawned"; return false; }
            if (NetPlayers::SlotIsFriendly(p.netSlot)) { *why = "friendly"; return false; }
            if (p.hasState && p.State().health <= 0.0f) { *why = "dead"; return false; }
            if (!p.Exists()) { *why = "no process"; return false; }
            return true;
        }

        float LockOnDistance()
        {
            const float base = s_swing.sprint ? kSprintLockOnDistanceM : kLockOnDistanceM;
            const float lead = std::clamp(s_swing.speed * kLockOnLeadSec, 0.0f, kLockOnLeadMaxM);
            return base + lead;
        }

        bool AcquireTarget(const Transform& me, Candidate& out)
        {
            out = Candidate{};
            float bestDist = LockOnDistance();
            int considered = 0;

            for (NetPlayer& p : NetPlayers::Remotes()) {
                const char* why = nullptr;
                if (!PuppetTargetable(p, &why)) {
                    TO1_LOG("[MELEE] acquire: slot%d rejected (%s)\n", p.netSlot, why);
                    continue;
                }
                ++considered;

                const Transform them = LOCATOR::GET_TRANSFORM(p.npcSid);
                if (!Finite(them.Position)) continue;

                const float dx = them.Position.x - me.Position.x;
                const float dz = them.Position.z - me.Position.z;
                const float dy = them.Position.y - me.Position.y;
                const float dist = me.hdistance(them);
                TO1_LOG("[MELEE] acquire: slot%d dist=%.2f dy=%.2f\n", p.netSlot, dist, dy);

                if (dist < kLockOnMinDistanceM || dist > bestDist) continue;
                if (std::fabs(dy) > kLockOnMaxHeightDeltaM) continue;

                out.slot = p.netSlot;
                out.dist = dist;
                out.dx = dx;
                out.dz = dz;
                bestDist = dist;
            }

            if (out.slot < 0) {
                TO1_LOG("[MELEE] acquire: no target (%d considered, range=%.2f sprint=%d)\n",
                    considered, LockOnDistance(), s_swing.sprint ? 1 : 0);
                return false;
            }
            return true;
        }
    }

    void ExemptMeleeTarget(StringId64 npcSid, float seconds)
    {
        s_exemptSid = npcSid;
        s_exemptTtl = npcSid ? seconds : 0.0f;
    }

    bool MeleeTargetExempt(StringId64 npcSid)
    {
        return npcSid != 0 && npcSid == s_exemptSid && s_exemptTtl > 0.0f;
    }

    int ExecutionTargetSlot()
    {
        return (s_execution.phase == Execution::Phase::Running) ? s_execution.slot : -1;
    }


    bool BeingExecuted() { return s_executedBy.active; }

    bool CommittedToExecutor() { return s_executedBy.active || s_promisedTo >= 0; }

    bool BeingExecutedBy(int attackerSlot)
    {
        return s_executedBy.active && attackerSlot >= 0 && attackerSlot == s_executedBy.slot;
    }

    bool PuppetFrozenForExecution(int slot)
    {
        if (slot < 0) return false;
        if (s_execution.phase == Execution::Phase::Running && slot == s_execution.slot) return true;
        if (s_executedBy.active && slot == s_executedBy.slot) return true;
        return false;
    }

    namespace {
        void FinishExecution(const char* why);
        void FinishBeingExecuted(const char* why);
        void BeginExecution();
    }

    void NotifyPuppetGone(int slot)
    {
        if (slot < 0) return;
        if (s_executedBy.active && s_executedBy.slot == slot)
            FinishBeingExecuted("attacker puppet destroyed");

        if (s_execution.phase != Execution::Phase::Idle && s_execution.slot == slot)
            FinishExecution("victim puppet destroyed");
    }

    void Reset()
    {
        if (s_executedBy.active) FinishBeingExecuted("session reset");
        if (s_execution.phase != Execution::Phase::Idle)
            FinishExecution("session reset");
        net::engine::StopMeleeAction(SID("player"));

        s_swing = SwingState{};
        s_execution = Execution{};
        s_executedBy = VictimExecution{};
        s_promisedTo = -1;
        s_promisedAge = 0.0f;
        s_executionCooldown = 0.0f;
        ExemptMeleeTarget(0, 0.0f);
    }

    bool QueueExecution(const NetPlayer& victim, uint64_t weapon, float damage)
    {
        if (!net::session::IsConnected()) return false;
        if (s_execution.phase != Execution::Phase::Idle) return false;
        if (!victim.spawned || !victim.npcSid) return false;
        if (victim.dead) return false;
        if (s_executedBy.active) return false;
        if (s_promisedTo >= 0) return false;

        s_execution = Execution{};
        s_execution.phase = Execution::Phase::Requested;
        s_execution.npcSid = victim.npcSid;
        s_execution.slot = victim.netSlot;
        s_execution.weapon = weapon;
        s_execution.damage = damage;

        ExecutionRequestEvent req{};
        req.victimSlot = static_cast<uint8_t>(victim.netSlot);
        net::session::SendEvent(NEVT_EXECUTION_REQUEST, req,
            static_cast<uint8_t>(victim.netSlot));

        TO1_LOG("[MELEE] lethal melee claim on slot%d -- waiting for their reply\n",
            victim.netSlot);

        return true;
    }

    void OnExecutionReply(int victimSlot, int replyCode)
    {
        if (s_execution.phase != Execution::Phase::Requested ||
            s_execution.slot != victimSlot)
            return;

        if (replyCode == kExecReplyRefusedMutual || replyCode == kExecReplyRefusedClaimed) {
            s_execution.forfeitKill = true;
            TO1_LOG("[MELEE] slot%d is owned by another finisher -- our claim forfeited\n",
                victimSlot);
            FinishExecution("the kill belongs to their executor");
            return;
        }

        if (replyCode != kExecReplyAccepted) {
            TO1_LOG("[MELEE] slot%d refused our execution -- landing it as a normal punch\n",
                victimSlot);
            FinishExecution("refused by the victim");
            return;
        }
        BeginExecution();
    }

    int AcceptIncomingExecution(int attackerSlot)
    {
        if (attackerSlot < 0 || attackerSlot >= kMaxNetPlayers) return kExecReplyRefused;

        if (PLAYER::IS_IN_STATE(SID("Death"))) {
            TO1_LOG("[MELEE] execution request from slot%d refused -- we are already dead\n",
                attackerSlot);
            return kExecReplyRefused;
        }

        if (s_executedBy.active) {
            if (s_executedBy.slot != attackerSlot)
                TO1_LOG("[MELEE] incoming execution from slot%d refused -- slot%d already has us\n",
                    attackerSlot, s_executedBy.slot);
            return kExecReplyRefusedClaimed;
        }

        if (s_promisedTo >= 0) {
            if (s_promisedTo != attackerSlot) {
                TO1_LOG("[MELEE] execution request from slot%d refused -- promised to slot%d\n",
                    attackerSlot, s_promisedTo);
                return kExecReplyRefusedClaimed;
            }
            s_promisedAge = 0.0f;
            return kExecReplyAccepted;
        }

        if (s_execution.phase == Execution::Phase::Running) {
            TO1_LOG("[MELEE] execution request from slot%d refused -- our own finisher is playing\n",
                attackerSlot);
            return kExecReplyRefused;
        }

        if (s_execution.phase != Execution::Phase::Idle) {
            if (s_execution.slot == attackerSlot) {
                const NetPlayer* me = NetPlayers::Local();
                const int mySlot = me ? me->netSlot : -1;
                if (mySlot >= 0 && mySlot < attackerSlot) {
                    TO1_LOG("[MELEE] mutual execution with slot%d -- ours wins, theirs refused\n",
                        attackerSlot);
                    return kExecReplyRefusedMutual;
                }

                s_execution.forfeitKill = true;
                TO1_LOG("[MELEE] mutual execution with slot%d -- theirs wins, ours forfeited\n",
                    attackerSlot);
            }
            else {
                TO1_LOG("[MELEE] slot%d is executing us -- abandoning our execution on slot%d\n",
                    attackerSlot, s_execution.slot);
            }
            FinishExecution("pre-empted by an incoming execution");
        }

        s_promisedTo = attackerSlot;
        s_promisedAge = 0.0f;
        return kExecReplyAccepted;
    }

    void OnExecutedBy(int attackerSlot, StringId64 attackerNpcSid, StringId64 moveSid)
    {
        s_promisedTo = -1;   // the promise is redeemed; the move is starting now
        s_promisedAge = 0.0f;
        s_executedBy = VictimExecution{};
        s_executedBy.playerPtr = NetPlayers::LocalPtr();
        s_executedBy.active = true;
        s_executedBy.slot = attackerSlot;
        s_executedBy.npcSid = attackerNpcSid;
        s_executedBy.moveSid = moveSid;

        NetPlayers::BySlot(attackerSlot).SuspendForEngineAction();

        TO1_LOG("[MELEE] being executed: attacker puppet slot%d handed to the engine action\n",
            attackerSlot);
    }

    namespace {
        void FinishExecution(const char* why)
        {
            const Execution exec = s_execution;
            s_execution = Execution{};
            ExemptMeleeTarget(0, 0.0f);
            if (exec.phase == Execution::Phase::Idle) return;

            if (exec.phase == Execution::Phase::Running)
                s_executionCooldown = kExecutionCooldownSec;

            TO1_LOG("[MELEE] execution on slot%d ended (%s)\n", exec.slot, why);

            net::engine::StopMeleeAction(SID("player"));

            if (!exec.syncSent && exec.slot >= 0 && exec.slot < kMaxNetPlayers) {
                NetPlayers::BySlot(exec.slot).ReclaimFromEngine();
                if (exec.forfeitKill) {
                    TO1_LOG("[MELEE] execution aborted -- kill on slot%d forfeited to the "
                        "attacker they accepted\n", exec.slot);
                }
                else if (exec.damage > 0.0f) {
                    TO1_LOG("[MELEE] execution aborted -- delivering the withheld %.1f kill on slot%d\n",
                        exec.damage, exec.slot);
                    net::hooks::ReportWithheldMeleeKill(exec.slot, exec.weapon, exec.damage);
                }
            }
        }

        void FinishBeingExecuted(const char* why)
        {
            const VictimExecution v = s_executedBy;
            s_executedBy = VictimExecution{};
            if (!v.active) return;

            TO1_LOG("[MELEE] being executed by slot%d ended (%s)\n", v.slot, why);

            if (v.slot >= 0 && v.slot < kMaxNetPlayers)
                NetPlayers::BySlot(v.slot).ResumeFromEngineAction();

            net::engine::StopMeleeAction(SID("player"));
            ExemptMeleeTarget(0, 0.0f);

            if (v.playerPtr && v.playerPtr == NetPlayers::LocalPtr())
                net::hooks::ForceLocalDeath(why);
            else
                TO1_LOG("[MELEE] not forcing death -- the local player was replaced mid-move\n");
        }

        // The victim agreed. THIS is where we commit: release their puppet and start the move.
        void BeginExecution()
        {
            const Execution exec = s_execution;
            if (!net::session::IsConnected()) { FinishExecution("no session"); return; }
            if (!kExecutionEnabled || s_executionCooldown > 0.0f) {
                FinishExecution("no finisher available");
                return;
            }

            if (PLAYER::IS_IN_STATE(SID("Death"))) { FinishExecution("we died first"); return; }
            if (s_executedBy.active || s_promisedTo >= 0) {
                const int owner = s_executedBy.active ? s_executedBy.slot : s_promisedTo;
                if (owner == exec.slot) s_execution.forfeitKill = true;
                FinishExecution("we are being executed");
                return;
            }
            if (!ENTITY::DOES_EXIST(SID("player")) || !ENTITY::DOES_EXIST(exec.npcSid)) {
                FinishExecution("target gone");
                return;
            }

            {
                const NetPlayer& victim = NetPlayers::BySlot(exec.slot);
                if (exec.slot < 0 || !victim.IsActiveRemote() || victim.npcSid != exec.npcSid ||
                    victim.dead || victim.dying ||
                    DC::Call<"character-is-dead?">(exec.npcSid) != 0) {
                    FinishExecution("victim died before the move started");
                    return;
                }
            }

            const Transform me = LOCATOR::GET_TRANSFORM(SID("player"));
            const Transform them = LOCATOR::GET_TRANSFORM(exec.npcSid);
            if (!Finite(me.Position) || !Finite(them.Position)) { FinishExecution("bad transform"); return; }

            const float dist = me.distance(them);
            if (dist > kExecutionMaxDistanceM) {
                TO1_LOG("[MELEE] execution skipped: slot%d is %.2f m away\n", exec.slot, dist);
                FinishExecution("out of range");
                return;
            }

            ExemptMeleeTarget(exec.npcSid, kExecutionMaxHoldSec + kExecutionMinHoldSec);

            NetPlayers::BySlot(exec.slot).ReleaseToEngine();

            const uint64_t started = DC::Call<"start-melee-action">(SID("player"), exec.npcSid, kExecutionMove, 0ull, 0ull, 1ull, 0ull, 0ull);
            TO1_LOG("[MELEE] execution %llx on slot%d at %.2f m -> started=%llu\n",
                static_cast<unsigned long long>(kExecutionMove), exec.slot, dist,
                static_cast<unsigned long long>(started));

            if (!started) { FinishExecution("engine refused the move"); return; }

            s_execution.phase = Execution::Phase::Running;
            s_execution.age = 0.0f;

            TO1_LOG("[MELEE] execution t=0: move now playing = %llx (sync half sent to slot%d)\n",
                static_cast<unsigned long long>(MELEE::GET_CURRENT_MELEE_ACTION(SID("player"))),
                exec.slot);
        }
    }

    void Tick(float dt)
    {
        const bool dtValid = std::isfinite(dt) && dt > 0.0f;
        if (dtValid) {
            s_executionCooldown = std::max(s_executionCooldown - dt, 0.0f);
            s_exemptTtl = std::max(s_exemptTtl - dt, 0.0f);
        }

        if (s_promisedTo >= 0 && !s_executedBy.active) {
            if (dtValid) s_promisedAge += dt;
            if (s_promisedAge > kExecutionPromiseTimeoutSec) {
                TO1_LOG("[MELEE] slot%d never sent the move it asked for -- promise lapsed\n",
                    s_promisedTo);
                s_promisedTo = -1;
                s_promisedAge = 0.0f;
            }
        }

        if (s_executedBy.active) {
            if (dtValid) s_executedBy.age += dt;

            const bool playerInMelee = DC::Call<"character-in-melee-attack?">(SID("player")) != 0;
            const bool npcInMelee = DC::Call<"character-in-melee-attack?">(s_executedBy.npcSid) != 0;

            if (playerInMelee && (s_executedBy.moveSid == 0 ||
                MELEE::GET_CURRENT_MELEE_ACTION(SID("player")) == s_executedBy.moveSid))
                s_executedBy.sawPlayerMelee = true;
            if (npcInMelee)    s_executedBy.sawNpcMelee = true;

            const bool myHalfDone = s_executedBy.sawPlayerMelee && !playerInMelee;
            const bool npcBusy = s_executedBy.sawNpcMelee && npcInMelee;

            if (!net::session::IsConnected() || !ENTITY::DOES_EXIST(s_executedBy.npcSid))
                FinishBeingExecuted("attacker gone");
            else if (s_executedBy.age > kExecutionMaxHoldSec)
                FinishBeingExecuted("timeout");                 // sanity net only
            else if (myHalfDone && !npcBusy)
                FinishBeingExecuted("animation done");
        }

        if (s_execution.phase == Execution::Phase::Idle) return;
        if (dtValid) s_execution.age += dt;

        if (s_execution.phase == Execution::Phase::Requested) {
            if (s_execution.age > kExecutionAckTimeoutSec)
                FinishExecution("no reply to the execution request");
            return;
        }

        if (!net::session::IsConnected() || !ENTITY::DOES_EXIST(s_execution.npcSid)) {
            FinishExecution("target gone");
            return;
        }
        if (s_execution.age > kExecutionMaxHoldSec) {
            FinishExecution("timeout");
            return;
        }

        const uint64_t move = MELEE::GET_CURRENT_MELEE_ACTION(SID("player"));
        const uint64_t defMove = MELEE::GET_CURRENT_MELEE_ACTION(s_execution.npcSid);
        const bool dead = DC::Call<"character-is-dead?">(s_execution.npcSid) != 0;
        if (move != s_lastLoggedMove || defMove != s_lastLoggedDefMove || dead != s_lastLoggedDead) {
            s_lastLoggedMove = move;
            s_lastLoggedDefMove = defMove;
            s_lastLoggedDead = dead;
            TO1_LOG("[MELEE] execution t=%.2f: attacker=%llx defender=%llx defenderDead=%d\n",
                s_execution.age, static_cast<unsigned long long>(move),
                static_cast<unsigned long long>(defMove), dead ? 1 : 0);
        }

        const bool inMelee = DC::Call<"character-in-melee-attack?">(SID("player")) != 0;
        if (inMelee) s_execution.sawMelee = true;


        if (!s_execution.syncSent) {
            const uint64_t sync =
                defMove ? defMove                                     // the victim's own half
                : (s_execution.sawMelee && !inMelee) ? kExecutionMove   // move over, never paired
                : (!s_execution.sawMelee && s_execution.age > kExecutionSyncGiveUpSec)
                ? kExecutionMove   // never started at all
                : 0;                                                    // mid-move -- keep waiting
            if (sync) {
                s_execution.syncSent = true;
                ExecutionEvent evt{};
                evt.victimSlot = static_cast<uint8_t>(s_execution.slot);
                evt.moveSID = sync;

                net::session::SendEvent(NEVT_PLAYER_EXECUTION, evt,
                    static_cast<uint8_t>(s_execution.slot));
                TO1_LOG("[MELEE] execution sync half -> slot%d: move %llx at t=%.2f%s\n",
                    s_execution.slot, static_cast<unsigned long long>(sync), s_execution.age,
                    defMove ? "" : " (fallback -- victim half never appeared)");
            }
        }

        if (s_execution.sawMelee && !inMelee)
            FinishExecution("animation done");
    }

    bool LockOnAttack(uintptr_t player, float* dir, float* stick, float* mag)
    {
        s_swing = SwingState{};
        if (!player || player != NetPlayers::LocalPtr()) return false;
        net::hooks::EnsureStealthDenyHook();
        if (!net::session::IsConnected()) return false;
        if (!ENTITY::DOES_EXIST(SID("player"))) return false;

        s_swing.sprint = DC::Call<"player-is-sprinting?">(1ull) != 0;
        s_swing.speed = floatFromUint64(DC::Call<"get-player-melee-run-accumulator">());
        if (!std::isfinite(s_swing.speed) || s_swing.speed < 0.0f) s_swing.speed = 0.0f;

        if (!kLockOnEnabled || !dir || !stick || !mag) return false;
        if (PLAYER::IS_IN_STATE(SID("Death"))) return false;

        const Transform me = LOCATOR::GET_TRANSFORM(SID("player"));
        if (!Finite(me.Position) || !Finite(me.Rotation)) return false;

        Candidate c;
        if (!AcquireTarget(me, c)) return false;

        const float targetYaw = std::atan2(c.dx, c.dz);
        const float curYaw = YawOf(me.Rotation);
        const float delta = WrapRad(targetYaw - curYaw);

        if (std::fabs(delta) * kRadToDeg >= 1.0f) {
            const Quaternion rot = Normalized(YawQuat(delta) * me.Rotation);
            if (Finite(rot))
                LOCATOR::SET_OBJECT_LOCATOR(SID("player"), MEMORY::ALLOC_BOUND_FRAME(me.Position, rot));
        }

        const float inLen = std::sqrt(dir[0] * dir[0] + dir[2] * dir[2]);
        const float len = (inLen > 1e-4f) ? inLen : 1.0f;
        dir[0] = std::sin(targetYaw) * len;
        dir[2] = std::cos(targetYaw) * len;

        stick[0] = std::sin(targetYaw);
        stick[2] = std::cos(targetYaw);
        mag[0] = mag[1] = mag[2] = mag[3] = std::max(mag[0], 1.0f);

        TO1_LOG("[MELEE] lock-on: slot%d dist=%.2f yaw %.1f -> %.1f (sprint=%d speed=%.2f)\n",
            c.slot, c.dist, curYaw * kRadToDeg, targetYaw * kRadToDeg,
            s_swing.sprint ? 1 : 0, s_swing.speed);
        return true;
    }

    uintptr_t CurrentMeleeActionOf(uintptr_t player)
    {
        if (!player) return 0;
        const uintptr_t vtable = *reinterpret_cast<uintptr_t*>(player);
        if (!vtable) return 0;
        const auto getAction = *reinterpret_cast<uintptr_t(__fastcall**)(uintptr_t)>(vtable + 3728);
        if (!getAction) return 0;
        const uintptr_t action = getAction(player);
        return (action > 0x10000) ? action : 0;
    }

    uintptr_t SubstituteMeleeTarget(uintptr_t player)
    {
        if (!kSubstituteMeleeTarget || !player || player != NetPlayers::LocalPtr()) return 0;
        if (!net::session::IsConnected()) return 0;
        if (!ENTITY::DOES_EXIST(SID("player"))) return 0;

        const Transform me = LOCATOR::GET_TRANSFORM(SID("player"));
        if (!Finite(me.Position) || !Finite(me.Rotation)) return 0;

        const float refYaw = YawOf(me.Rotation);
        uintptr_t best = 0;
        int bestSlot = -1;
        float bestDist = s_swing.sprint ? kSprintSubstituteRadiusM : kSubstituteRadiusM;

        for (NetPlayer& p : NetPlayers::Remotes()) {
            const char* why = nullptr;
            if (!PuppetTargetable(p, &why)) continue;

            const Transform them = LOCATOR::GET_TRANSFORM(p.npcSid);
            if (!Finite(them.Position)) continue;

            const float dx = them.Position.x - me.Position.x;
            const float dz = them.Position.z - me.Position.z;
            const float dy = them.Position.y - me.Position.y;
            const float dist = me.hdistance(them);
            if (dist > bestDist) continue;
            if (std::fabs(dy) > kSubstituteMaxHeightDeltaM) continue;
            if (std::fabs(WrapRad(std::atan2(dx, dz) - refYaw)) * kRadToDeg > kSubstituteConeHalfAngleDeg) continue;

            const uintptr_t ptr = p.Ptr();
            if (!ptr) continue;
            best = ptr;
            bestSlot = p.netSlot;
            bestDist = dist;
        }

        if (best)
            TO1_LOG("[MELEE] substitute target: slot%d at %.2f m (engine had none)\n", bestSlot, bestDist);
        return best;
    }

    float ResolveMeleeDamage(float engineDamage)
    {
        if (kMeleeDamageOverride > 0.0f) return kMeleeDamageOverride;
        if (std::isfinite(engineDamage) && engineDamage > kMeleeDamageMax) {
            TO1_LOG("[MELEE] engine melee damage %.1f exceeds %.1f -- treating as a scripted value, using %.1f\n",
                engineDamage, kMeleeDamageMax, kMeleeDamageFallback);
            return kMeleeDamageFallback;
        }
        if (std::isfinite(engineDamage) && engineDamage > 0.0f) return engineDamage;
        return kMeleeDamageFallback;
    }

    void PlayLocalHitFeedback(NetPlayer& victim, bool lethal)
    {
        if (!victim.spawned || !victim.npcSid || !victim.Exists()) return;
        if (!ENTITY::DOES_EXIST(SID("player"))) return;

        Transform them = LOCATOR::GET_TRANSFORM(victim.npcSid);
        if (!Finite(them.Position)) return;
        them.Position.y += kMeleeHitSoundHeightM;

        static size_t s_next = 0;
        static size_t s_nextLethal = 0;
        constexpr size_t kCount = sizeof(kMeleeHitSfx) / sizeof(kMeleeHitSfx[0]);
        constexpr size_t kLethalCount = sizeof(kMeleeLethalHitSfx) / sizeof(kMeleeLethalHitSfx[0]);
        StringId64 sfx;
        if (lethal) {
            sfx = kMeleeLethalHitSfx[s_nextLethal];
            s_nextLethal = (s_nextLethal + 1) % kLethalCount;
        }
        else {
            sfx = kMeleeHitSfx[s_next];
            s_next = (s_next + 1) % kCount;
        }

        AUDIO::PLAY_SOUND_AT_POSITION(sfx, MEMORY::ALLOC_VECTOR(them.Position));
        TO1_LOG("[MELEE] hit sfx %llx at (%.2f %.2f %.2f)\n", static_cast<unsigned long long>(sfx),
            them.Position.x, them.Position.y, them.Position.z);
    }
}
