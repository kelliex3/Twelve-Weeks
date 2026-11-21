#include "net-hooks.hpp"
#include "net-player.hpp"
#include "net-engine.hpp"
#include "net-session.hpp"
#include "net-replication.hpp"
#include "net-patches.hpp"
#include "net-debug.hpp"
#include "kill-feed.hpp"
#include <intrin.h>
#include "net-gui2.hpp"
#include "net-matchmaking.hpp"
#include "match.hpp"
#include "gameplay/perks/perk-registry.hpp"
#include "gameplay/barrel-check.hpp"
#include "gameplay/melee.hpp"
#include "music-tier.hpp"

#include <bit>
#include <cmath>

namespace net::hooks {

    using net::match::kForceKillDamage;

    constexpr float kPendingLaunchTtlSec = 1.0f;

    static float Length3(const float* v)
    {
        return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    }

    static NetPlayer* RemoteByPtr(uintptr_t v)
    {
        if (!v || v < 0x10000) return nullptr;
        NetPlayer* p = NetPlayers::FromEnginePtr(v);
        return (p && !p->IsLocal()) ? p : nullptr;
    }

    static uintptr_t AttackerFromAttackInfo(uintptr_t attackInfo)
    {
        if (uintptr_t handle = *reinterpret_cast<uintptr_t*>(attackInfo + 8)) {
            uintptr_t cand = *reinterpret_cast<uintptr_t*>(handle + 8);
            if (cand && *reinterpret_cast<uint32_t*>(cand + 148) ==
                *reinterpret_cast<uint32_t*>(attackInfo + 16))
                return cand;
        }
        return 0;
    }

    static void WriteBytes(uintptr_t addr, const uint8_t* bytes, size_t n)
    {
        DWORD old;
        VirtualProtect(reinterpret_cast<void*>(addr), n, PAGE_EXECUTE_READWRITE, &old);
        memcpy(reinterpret_cast<void*>(addr), bytes, n);
        VirtualProtect(reinterpret_cast<void*>(addr), n, old, &old);
    }

    static SafetyHookMid g_hkShot;
    static void hk_Shot(SafetyHookContext& ctx)
    {
        uintptr_t weapon = ctx.rdi;
        uintptr_t fireArgs = ctx.r14;
        if (!weapon || !fireArgs || !net::session::IsConnected()) return;

        uintptr_t local = NetPlayers::LocalPtr();
        if (!local) return;
        if (*reinterpret_cast<uint32_t*>(fireArgs + 0x48) !=
            *reinterpret_cast<uint32_t*>(local + 0x94))
            return;   // not our shot (e.g. a remote puppet re-firing)

        FireShotEvent evt{ 0 };
        // note: evt.ori and evt.org are never read back so its fine to pass a "dummy" empty event over the network. in the future, should anyone read back evt.ori and evt.org,
        // ShotBlocked should also set the ori/org correctly to the blocked shot data.
        // evt.launchVel IS read back: a zeroed one means "no charge latch", which is the correct
        // thing to replicate for a blocked shot.
        if (gameplay::barrelcheck::ShotBlocked()) {
            TO1_LOG("[BARREL] local shot while blocked -> NEVT_FIRE_SHOT dummy zeroed\n");
        }
        else {
            memcpy(evt.ori, reinterpret_cast<void*>(weapon + 0xF10), sizeof(evt.ori));
            memcpy(evt.org, reinterpret_cast<void*>(weapon + 0xF20), sizeof(evt.org));

            if (*reinterpret_cast<uint8_t*>(weapon + 0xF30)) {
                float launchVel[4];
                memcpy(launchVel, reinterpret_cast<void*>(weapon + 0xF40), sizeof(launchVel));
                if (std::isfinite(launchVel[0]) && std::isfinite(launchVel[1]) &&
                    std::isfinite(launchVel[2]))
                    memcpy(evt.launchVel, launchVel, sizeof(evt.launchVel));
            }
        }

        net::session::SendEvent(NEVT_FIRE_SHOT, evt);
        TO1_LOG("[FIRE] captured local shot -> broadcast NEVT_FIRE_SHOT\n");
    }

    static SafetyHookMid g_hkFireCue;
    static volatile long g_dFireCue = 0;   // shots where we forced the player cue
    static void hk_FireCue(SafetyHookContext& ctx) // Allows npc puppets to play the player fire cue on their weapons, so that the player cue is heard on remote clients.
    {
        if (!net::session::IsConnected()) return;
        const uintptr_t slot = ctx.rbp - 0x50;
        if (slot < 0x10000) return;
        const uintptr_t holder = *reinterpret_cast<uintptr_t*>(slot);
        if (!RemoteByPtr(holder)) return;   // not one of our puppets -> engine decides as normal

        ctx.rcx |= 1;   // cl = 1 -> "held by a Player" -> selects [settings+0x68], the player cue

        const long n = _InterlockedIncrement(&g_dFireCue);
        if (n == 1 || (n % 50) == 0)
            TO1_LOG("[FIRE] forced player fire cue on puppet weapon (%ld shots)\n", n);
    }

    static SafetyHookInline g_hkProjectileThrow;
    static bool __fastcall hk_ProjectileThrow(uintptr_t player, uintptr_t weapon,
        uintptr_t fromPos, uintptr_t toPos,
        uintptr_t velocity)
    {
        uintptr_t local = NetPlayers::LocalPtr();
        if (local && player == local && net::session::IsRunning()) {
            float* f = reinterpret_cast<float*>(fromPos);
            float* t = reinterpret_cast<float*>(toPos);
            float* v = reinterpret_cast<float*>(velocity);

            ProjectileThrowEvent evt{};
            if (f) { evt.fromPos[0] = f[0]; evt.fromPos[1] = f[1]; evt.fromPos[2] = f[2]; }
            if (t) { evt.toPos[0] = t[0]; evt.toPos[1] = t[1]; evt.toPos[2] = t[2]; }
            if (v) { evt.vel[0] = v[0]; evt.vel[1] = v[1]; evt.vel[2] = v[2]; }
            evt.throwableSID = net::engine::GetPlayerWeaponId(local);
            net::session::SendEvent(NEVT_THROWPROJECTILE, evt);
            TO1_LOG("[NET] threw projectile %llx\n",
                static_cast<unsigned long long>(evt.throwableSID));
        }
        return g_hkProjectileThrow.call<bool>(player, weapon, fromPos, toPos, velocity);
    }

    static SafetyHookInline g_hkDoorOpen;
    static void __fastcall hk_DoorOpen(uintptr_t a1, uintptr_t door, uint64_t animKey, char a4, float a5, float a6, float a7)
    {
        if (door && net::session::IsRunning()) {
            DoorOpenEvent evt{};
            evt.doorSid = *reinterpret_cast<uint64_t*>(door + 0xC0);
            evt.stateSid = 0;
            if (evt.doorSid) {
                uintptr_t player = *reinterpret_cast<uintptr_t*>(a1 + 8);
                uintptr_t playerAn = player ? *reinterpret_cast<uintptr_t*>(player + 0x1B8) : 0;
                uintptr_t doorAn = *reinterpret_cast<uintptr_t*>(door + 0x1B8);
                if (playerAn && doorAn) {
                    auto remap = GameFn<void* (__fastcall*)(uintptr_t, uint64_t*, uint64_t)>(0x1444D80);
                    auto lookup = GameFn<uintptr_t(__fastcall*)(uintptr_t, void*, uint64_t)>(0x1444A80);
                    uint64_t leaf = 0;
                    remap(playerAn, &leaf, animKey);
                    if (leaf) {
                        alignas(16) uint8_t buf[32] = {};
                        uintptr_t res = lookup(doorAn, buf, leaf);
                        if (res && *reinterpret_cast<uint64_t*>(res + 16))
                            evt.stateSid = leaf;
                    }
                }
                TO1_LOG("[DOOR] open sid=%llx state=%llx%s\n",
                    static_cast<unsigned long long>(evt.doorSid),
                    static_cast<unsigned long long>(evt.stateSid),
                    evt.stateSid ? "" : "  (unresolved -> receiver default)");
                net::session::SendEvent(NEVT_OPEN_DOOR, evt);
            }
        }
        g_hkDoorOpen.call<void>(a1, door, animKey, a4, a5, a6, a7);
    }


    static SafetyHookInline g_hkShouldSprint;
    static char __fastcall hk_ShouldSprint(uintptr_t sprintCtrl)
    {
        uintptr_t player = *reinterpret_cast<uintptr_t*>(sprintCtrl + 8);
        if (NetPlayer* p = RemoteByPtr(player); p && p->hasState && p->State().isSprinting)
            return 1;
        return g_hkShouldSprint.call<char>(sprintCtrl);
    }

    static bool ReportLocalHitOnPuppet(NetPlayer& puppet, uint64_t weapon, uint64_t ammo, float damage, bool headshot)
    {
        const float ownerHp = puppet.State().health;
        const HealthLedger::HitPrediction hit =
            puppet.ledger.OnDamageDealt(damage, ownerHp, net::replication::NowSec());
        const bool predictedLethal = hit.lethal;

        TO1_LOG("[NET] hit \"%s\" with %llx for %.1f (pred %d -> %d, they report %d)%s%s\n",
            puppet.name, static_cast<unsigned long long>(weapon), damage,
            static_cast<int>(hit.beforeHp), static_cast<int>(hit.afterHp),
            static_cast<int>(ownerHp),
            headshot ? " HEADSHOT" : "", predictedLethal ? " KILL?" : "");

        PlayerDamageEvent dmg{ weapon, ammo,
                               predictedLethal ? kForceKillDamage : damage,
                               static_cast<uint8_t>(predictedLethal ? 1 : 0),
                               static_cast<uint8_t>(headshot ? 1 : 0) };
        net::session::SendEvent(NEVT_TAKE_DAMAGE, dmg, static_cast<uint8_t>(puppet.netSlot));

        // ext HitTickHook: a damaging hit we land is a music input ("we hit an enemy", 3.0).
        // This is our outgoing counterpart to the "we took a hit" input on the receive side.
        if (damage > 0.0f && damage < 100000.0f)
            net::audio::music_tier::NoteEvent(3.0f, "we hit an enemy");

        gameplay::HitContext hitCtx;
        hitCtx.victimSlot = puppet.netSlot;
        hitCtx.weaponSid = weapon;
        hitCtx.headshot = headshot;
        hitCtx.lethal = predictedLethal;
        gameplay::perks::NotifyDealHit(hitCtx);
        return predictedLethal;
    }

    void ReportWithheldMeleeKill(int netSlot, uint64_t weapon, float damage)
    {
        if (netSlot < 0 || netSlot >= kMaxNetPlayers) return;
        NetPlayer& puppet = NetPlayers::BySlot(netSlot);
        if (!puppet.IsActiveRemote() || !puppet.hasState) return;

        if (puppet.dead || net::match::Get().slotState[netSlot].dead) return;

        ReportLocalHitOnPuppet(puppet, weapon, 0, damage, false);
    }

    static uintptr_t g_deathBlowVictim = 0;
    static uintptr_t g_deathBlowHealth = 0;

    void SetDeathBlowVictim(uintptr_t victim)
    {
        g_deathBlowVictim = victim;
        g_deathBlowHealth = victim ? *reinterpret_cast<uintptr_t*>(victim + 0x610) : 0;
    }

    static SafetyHookInline g_hkBulletHit;
    static void __fastcall hk_BulletHitVictim(uintptr_t ctx, uintptr_t victim, uintptr_t attackInfo, char flags)
    {
        if (g_deathBlowVictim && victim == g_deathBlowVictim) {
            g_hkBulletHit.fastcall<void>(ctx, victim, attackInfo, flags);
            return;
        }
        if (attackInfo && flags != 0 && gameplay::barrelcheck::ImpactBlocked(ctx)) {
            uintptr_t denyLocal = NetPlayers::LocalPtr();
            if (denyLocal && AttackerFromAttackInfo(attackInfo) == denyLocal) {
                TO1_LOG("[BARREL] DENIED impact victim=%llx dmg=%.1f weapon=%llx\n",
                    static_cast<unsigned long long>(victim),
                    *reinterpret_cast<float*>(attackInfo + 64),
                    static_cast<unsigned long long>(
                        *reinterpret_cast<uint64_t*>(attackInfo + 32)));
                gameplay::barrelcheck::NotifyDeniedImpact();
                return;
            }
        }

        NetPlayer* puppet = (victim && net::session::IsConnected()) ? RemoteByPtr(victim) : nullptr;
        if (puppet && puppet->EngineReleasing()) puppet = nullptr;

        bool predictedLethal = false;   // our ledger says this shot kills -> force it + draw the red reticle

        if (puppet && attackInfo) {
            uintptr_t local = NetPlayers::LocalPtr();

            // Resolve the attacker process from the AttackInfo's handle (+8) + instance id (+16).
            uintptr_t attackerObj = AttackerFromAttackInfo(attackInfo);

            // Friendly fire
            const int ffMyTeam = net::matchmaking::MyTeam();
            const int ffVicTeam = net::match::Get().TeamOf(puppet->netSlot);
            const bool ffTeammate = (ffMyTeam >= 0 && ffVicTeam >= 0 && ffVicTeam == ffMyTeam);

            if (ffTeammate && local && attackerObj == local)
                return;

            if (local && attackerObj == local && !ffTeammate &&
                gameplay::melee::ExecutionTargetSlot() != puppet->netSlot) {
                uint64_t weapon = *reinterpret_cast<uint64_t*>(attackInfo + 32);
                uint64_t ammo = *reinterpret_cast<uint64_t*>(attackInfo + 40);
                float    damage = *reinterpret_cast<float*>(attackInfo + 64);

                if (uint64_t heldWeapon = net::engine::GetPlayerWeaponId(local))
                    weapon = heldWeapon;

                // headshot: AttackInfo+2484 == 0x9.
                const bool headshot = (*reinterpret_cast<uint8_t*>(attackInfo + 2484) == 0x9);
                predictedLethal = ReportLocalHitOnPuppet(*puppet, weapon, ammo, damage, headshot);
            }
        }

        if (puppet && puppet->npcSid) {
            if (attackInfo) {
                float* pDamage = reinterpret_cast<float*>(attackInfo + 64);     // AttackInfo.damage
                uint8_t* pHitFlags = reinterpret_cast<uint8_t*>(attackInfo + 2484); // hit-type flags
                const float   realDamage = *pDamage;
                const uint8_t realFlags = *pHitFlags;

                *pDamage = 0.0f; // puppet takes no damage, so never dies

                if (predictedLethal)
                    *pHitFlags = static_cast<uint8_t>(realFlags | 0x20);

                g_hkBulletHit.fastcall<void>(ctx, victim, attackInfo, flags); // -> reticle + FX, no death

                *pHitFlags = realFlags;
                *pDamage = realDamage;
            }
            return;
        }

        g_hkBulletHit.fastcall<void>(ctx, victim, attackInfo, flags);
    }

    static NetPlayer* PuppetOwningHealth(uintptr_t health)
    {
        if (!health || !net::session::IsConnected()) return nullptr;
        for (NetPlayer& p : NetPlayers::Remotes()) {
            if (p.EngineReleasing()) continue;   // released: its damage must reach the engine
            const uintptr_t npc = p.Ptr();
            if (npc && *reinterpret_cast<uintptr_t*>(npc + 0x610) == health)
                return &p;
        }
        return nullptr;
    }


    static SafetyHookInline g_hkHealthApplyDamage;
    static char __fastcall hk_HealthApplyDamage(uintptr_t health, float amount,
        char a3, char a4, char a5, char a6)
    {
        if (g_deathBlowHealth && health == g_deathBlowHealth)
            return g_hkHealthApplyDamage.fastcall<char>(health, amount, a3, a4, a5, a6);

        const int executed = gameplay::melee::ExecutionTargetSlot();
        if (amount > 0.0f) {
            if (NetPlayer* puppet = PuppetOwningHealth(health)) {
                if (puppet->netSlot == executed) {
                    TO1_LOG("[NET] blocked %.1f local damage to executed puppet \"%s\"\n",
                        amount, puppet->name);
                    amount = 0.0f;
                }
                else if (amount >= gameplay::melee::tuning::kMeleeDamageMax) {
                    TO1_LOG("[NET] blocked leaked finisher damage %.1f on puppet \"%s\"\n",
                        amount, puppet->name);
                    amount = 0.0f;
                    puppet->lastSyncedHealth = -1.0f;
                }
                else {
                    TO1_LOG("[NET] %.1f local health damage to puppet \"%s\" (exec slot %d)\n",
                        amount, puppet->name, executed);
                }
            }
        }


        return g_hkHealthApplyDamage.fastcall<char>(health, amount, a3, a4, a5, a6);
    }

    static SafetyHookInline g_hkCompositeMotionApply;
    static char __fastcall hk_CompositeMotionApply(uintptr_t compositeBody, uintptr_t a2, uintptr_t a3)
    {
        if (!compositeBody || *reinterpret_cast<uintptr_t*>(compositeBody + 8) == 0)
            return 0;
        return g_hkCompositeMotionApply.unsafe_fastcall<char>(compositeBody, a2, a3);
    }

    static SafetyHookInline g_hkAnimLayerCurInst;
    static uintptr_t __fastcall hk_AnimLayerCurInst(uintptr_t animStateLayer)
    {
        if (!animStateLayer)
            return 0;
        return g_hkAnimLayerCurInst.unsafe_fastcall<uintptr_t>(animStateLayer);
    }


    static SafetyHookInline g_hkJointLocator;
    static SafetyHookInline g_hkJointEval;
    static volatile long    g_dJointOob = 0;   // times a bad joint index was intercepted
    alignas(32) static float g_identityLocator[8] = { 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f };

    static inline bool JointIdxOk(uintptr_t jointCache, unsigned int idx)
    {
        if (!jointCache) return false;
        const uint64_t n = *reinterpret_cast<const uint64_t*>(jointCache + 0x48);
        return n != 0 && static_cast<uint64_t>(idx) < n;
    }

    static void NoteJointOob(unsigned int idx, uintptr_t jointCache)
    {
        const long hits = _InterlockedIncrement(&g_dJointOob);
        if (hits == 1 || (hits % 600) == 0) {   // once, then ~once per 10s of a 60Hz offender
            const uint64_t n = jointCache ? *reinterpret_cast<const uint64_t*>(jointCache + 0x48) : 0;
            TO1_LOG("[JOINTGUARD] blocked out-of-range joint %u (cache has %llu joints) (%ld total)\n",
                idx, static_cast<unsigned long long>(n), hits);
        }
    }

    static uintptr_t __fastcall hk_JointLocator(uintptr_t jointCache, unsigned int jointIdx, double blend)
    {
        if (JointIdxOk(jointCache, jointIdx))
            return g_hkJointLocator.unsafe_fastcall<uintptr_t>(jointCache, jointIdx, blend);
        NoteJointOob(jointIdx, jointCache);
        return reinterpret_cast<uintptr_t>(g_identityLocator);
    }

    static uintptr_t __fastcall hk_JointEval(uintptr_t jointCache, unsigned int jointIdx, double blend)
    {
        if (JointIdxOk(jointCache, jointIdx))
            return g_hkJointEval.unsafe_fastcall<uintptr_t>(jointCache, jointIdx, blend);
        NoteJointOob(jointIdx, jointCache);
        return 0;
    }

    static SafetyHookInline g_hkHideUninvolved;
    static void __fastcall hk_HideUninvolvedOnDeath(uintptr_t deathState, uintptr_t dyingChar)
    {
        if (net::session::IsRunning())
            return;
        g_hkHideUninvolved.fastcall<void>(deathState, dyingChar);
    }

    // Detect melee actions and broadcast them to the session
    static SafetyHookInline g_hkTryAttack;
    static char __fastcall hk_TryAttack(
        uintptr_t controller, void* dirVec, void* posVec, void* scalar,
        void* cmdReq, uint64_t sid, uintptr_t processAction, char* outBool,
        char a9, uintptr_t attackList, char a11, uintptr_t moveUniqueId)
    {
        uintptr_t player = controller ? *reinterpret_cast<uintptr_t*>(controller + 280) : 0;
        uintptr_t local = NetPlayers::LocalPtr();

        if (player && player == local && net::session::IsRunning()) {
            if (gameplay::melee::BeingExecuted() || PLAYER::IS_IN_STATE(SID("Death")))
                return 0;
        }

        alignas(16) float lockDir[4] = {};
        alignas(16) float lockStick[4] = {};
        alignas(16) float lockMag[4] = {};
        if (dirVec && player && player == local && net::session::IsRunning()) {
            memcpy(lockDir, dirVec, sizeof(lockDir));
            if (posVec) memcpy(lockStick, posVec, sizeof(lockStick));
            if (scalar) memcpy(lockMag, scalar, sizeof(lockMag));

            if (gameplay::melee::LockOnAttack(player, lockDir, lockStick, lockMag)) {
                dirVec = lockDir;
                posVec = lockStick;
                scalar = lockMag;
            }
        }

        char result = g_hkTryAttack.fastcall<char>(
            controller, dirVec, posVec, scalar, cmdReq, sid,
            processAction, outBool, a9, attackList, a11, moveUniqueId);

        if (result && player && player == local && net::session::IsRunning()) {
            if (StringId64 action = MELEE::GET_CURRENT_MELEE_ACTION(SID("player"))) {
                net::session::SendEventSid(NEVT_PLAYER_MELEE_ATTACK, action);
                TO1_LOG("[NET] melee action %llx broadcast\n",
                    static_cast<unsigned long long>(action));
            }
        }
        return result;
    }

    static float HalfToFloat(uint16_t h)
    {
        const uint32_t sign = static_cast<uint32_t>(h & 0x8000) << 16;
        const uint32_t exp = (h >> 10) & 0x1F;
        const uint32_t mant = h & 0x3FF;
        if (exp == 0) {
            if (mant == 0) return std::bit_cast<float>(sign);
            float v = static_cast<float>(mant) * (1.0f / 1024.0f) * 5.9604645e-8f * 1024.0f;
            return (sign ? -v : v);
        }
        if (exp == 31) return std::bit_cast<float>(sign | 0x7F800000u | (mant << 13));
        return std::bit_cast<float>(sign | ((exp + 112) << 23) | (mant << 13));
    }

    static uint16_t FloatToHalf(float f)
    {
        const uint32_t bits = std::bit_cast<uint32_t>(f);
        const uint32_t sign = (bits >> 16) & 0x8000;
        const int32_t  exp = static_cast<int32_t>((bits >> 23) & 0xFF) - 127 + 15;
        const uint32_t mant = bits & 0x7FFFFF;
        if (exp <= 0) return static_cast<uint16_t>(sign);
        if (exp >= 31) return static_cast<uint16_t>(sign | 0x7BFF);
        return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13));
    }

    static SafetyHookInline g_hkMeleeDamageRadius;
    static void __fastcall hk_MeleeDamageTargetInRadius(uintptr_t action, uintptr_t meleeEvent, uintptr_t target, float radius, float height)
    {
        using namespace gameplay::melee::tuning;

        bool ours = false;
        if (kSubstituteMeleeTarget && action && meleeEvent && net::session::IsConnected()) {
            const uintptr_t local = NetPlayers::LocalPtr();
            ours = local && gameplay::melee::CurrentMeleeActionOf(local) == action;
            if (ours && !target)
                target = gameplay::melee::SubstituteMeleeTarget(local);
        }

        if (!ours || !target) {
            g_hkMeleeDamageRadius.fastcall<void>(action, meleeEvent, target, radius, height);
            return;
        }

        uint16_t* pRadius = nullptr;
        uint16_t* pHeight = nullptr;
        uint16_t  savedRadius = 0;
        uint16_t  savedHeight = 0;
        if (const uintptr_t data = *reinterpret_cast<uintptr_t*>(meleeEvent + kMeleeEventDataOffset)) {
            pRadius = reinterpret_cast<uint16_t*>(data + kMeleeEventRadiusOffset);
            pHeight = reinterpret_cast<uint16_t*>(data + kMeleeEventHeightOffset);
            savedRadius = *pRadius;
            savedHeight = *pHeight;
            if (kMeleeEventRadiusScale != 1.0f)
                *pRadius = FloatToHalf(HalfToFloat(savedRadius) * kMeleeEventRadiusScale);
            if (kMeleeEventHeightScale != 1.0f)
                *pHeight = FloatToHalf(HalfToFloat(savedHeight) * kMeleeEventHeightScale);
            TO1_LOG("[MELEE] damage window: radius %.2f -> %.2f height %.2f -> %.2f\n",
                HalfToFloat(savedRadius), HalfToFloat(*pRadius),
                HalfToFloat(savedHeight), HalfToFloat(*pHeight));
        }

        g_hkMeleeDamageRadius.fastcall<void>(action, meleeEvent, target, radius, height);

        if (pRadius) *pRadius = savedRadius;
        if (pHeight) *pHeight = savedHeight;
    }

    static SafetyHookInline g_hkStealthAvail;
    static void __fastcall hk_StealthAvail(uintptr_t meleeControl, uint8_t* out,
        uintptr_t npc, uint64_t* a4, int a5)
    {
        g_hkStealthAvail.fastcall<void>(meleeControl, out, npc, a4, a5);

        if (!out || !npc) return;
        if (!RemoteByPtr(npc)) return;

        out[16] = 0;
    }

    static NetPlayer* RemoteByNpcSid(uint64_t sid)
    {
        if (!sid) return nullptr;
        for (NetPlayer& p : NetPlayers::Remotes())
            if (p.npcSid == sid) return &p;
        return nullptr;
    }

    static SafetyHookInline g_hkMeleeTargetFilter;
    static char __fastcall hk_MeleeTargetFilter(uintptr_t snapshot, uintptr_t player, void* a3, float a4,
        uint32_t* a5, uint32_t a6, uintptr_t a7, uint32_t* a8, uintptr_t a9, uint32_t* a10,
        uint64_t a11, uint32_t a12)
    {
        if (snapshot > 0x10000) {
            const uint64_t sid = *reinterpret_cast<const uint64_t*>(snapshot + 32);
            if (RemoteByNpcSid(sid) && !gameplay::melee::MeleeTargetExempt(sid))
                return 0;
        }

        return g_hkMeleeTargetFilter.fastcall<char>(snapshot, player, a3, a4, a5, a6, a7, a8,
            a9, a10, a11, a12);
    }

    static SafetyHookInline g_hkFindMeleeMove;
    static constexpr uintptr_t kMeleeSearchCharSets = 0x40;
    static constexpr uintptr_t kMeleeSearchDebugContext = 0x4A50;
    static bool __fastcall hk_FindMeleeMove(uintptr_t search, uintptr_t start)
    {
        if (search) {
            using SetFn = uintptr_t(__fastcall*)(uintptr_t);
            static const auto getAttackerSet = reinterpret_cast<SetFn>(g_base + 0xE51870);
            static const auto getProcess = reinterpret_cast<SetFn>(g_base + 0xE51920);
            const uintptr_t attacker = getAttackerSet(search + kMeleeSearchCharSets);
            if (attacker && !getProcess(attacker)) {
                const char* context = *reinterpret_cast<const char* const*>(search + kMeleeSearchDebugContext);
                TO1_LOG("[MELEE] FindMeleeMove denied: attacker set has no live process (%s)\n",
                    context ? context : "?");
                return false;
            }
        }
        return g_hkFindMeleeMove.fastcall<bool>(search, start);
    }

    //  PUPPET GUARDS
    static NetPlayer* LivePuppet(uintptr_t npc)
    {
        if (!npc || !net::session::IsConnected()) return nullptr;
        NetPlayer* p = RemoteByPtr(npc);
        return (p && !p->dead && !p->EngineOwned()) ? p : nullptr;
    }

    enum GuardId {
        kGuardRoot, kGuardTeleport, kGuardLegIk, kGuardFootPlant,
        kGuardPathTap, kGuardHandle, kGuardComposite, kGuardCount
    };
    static volatile long g_guardHits[kGuardCount] = {};

    static bool GuardHit(GuardId id, long every = 0)
    {
        const long n = _InterlockedIncrement(&g_guardHits[id]);
        return n == 1 || (every > 0 && (n % every) == 0);
    }

    static SafetyHookInline g_hkUpdateRootLocator;

    static void __fastcall hk_UpdateRootLocator(uintptr_t npc)
    {
        if (LivePuppet(npc)) {
            memcpy(reinterpret_cast<void*>(npc + 0x1E20),
                reinterpret_cast<const void*>(npc + 0x130), 32);
            if (GuardHit(kGuardRoot))
                TO1_LOG("[GUARD] puppet root-locator is ours alone now\n");
            return;
        }
        g_hkUpdateRootLocator.fastcall<void>(npc);
    }

    static SafetyHookInline g_hkTeleportToBoundFrame;
    static thread_local int t_puppetTeleportAllowed = 0;

    static void __fastcall hk_TeleportToBoundFrame(uintptr_t npc, uintptr_t bf,
        uint32_t a3, char a4, uint8_t a5)
    {
        if (!t_puppetTeleportAllowed) {
            if (const NetPlayer* p = LivePuppet(npc)) {
                if (GuardHit(kGuardTeleport, 60))
                    TO1_LOG("[GUARD] blocked an engine teleport of slot %d\n", p->netSlot);
                return;
            }
        }
        g_hkTeleportToBoundFrame.fastcall<void>(npc, bf, a3, a4, a5);
    }

    PuppetTeleportScope::PuppetTeleportScope() { ++t_puppetTeleportAllowed; }
    PuppetTeleportScope::~PuppetTeleportScope() { --t_puppetTeleportAllowed; }

    static SafetyHookInline g_hkNpcLegIkMode;

    static int __fastcall hk_NpcLegIkMode(uintptr_t legMod, float* outBlendTime)
    {
        uintptr_t npc = 0;
        if (legMod) {
            const uintptr_t h = *reinterpret_cast<uintptr_t*>(legMod + 0x38);
            const uint32_t  id = *reinterpret_cast<uint32_t*>(legMod + 0x40);
            if (h && id) {
                const uintptr_t owner = *reinterpret_cast<uintptr_t*>(h + 8);
                if (owner && *reinterpret_cast<uint32_t*>(owner + 0x94) == id) npc = owner;
            }
        }

        if (LivePuppet(npc)) {
            if (outBlendTime) *outBlendTime = 0.2f;   // the engine's default, 0x1406A31C0
            if (GuardHit(kGuardLegIk))
                TO1_LOG("[GUARD] puppet leg-IK mode forced to 0\n");
            return 0;
        }
        return g_hkNpcLegIkMode.fastcall<int>(legMod, outBlendTime);
    }
    static SafetyHookInline g_hkNpcFootPlantParams;

    static uintptr_t __fastcall hk_NpcFootPlantParams(uintptr_t out, uintptr_t npc)
    {
        if (out && LivePuppet(npc)) {
            *reinterpret_cast<uint32_t*>(out + 0) = 0x00000000u;   // weight 0.0f
            *reinterpret_cast<uint32_t*>(out + 4) = 0x3E4CCCCDu;   // blend  0.2f
            *reinterpret_cast<uint32_t*>(out + 8) = 0x00000000u;
            *reinterpret_cast<uint32_t*>(out + 12) = 0x00000000u;
            *reinterpret_cast<uint32_t*>(out + 16) = 0x3D4CCCCDu;   // exit   0.05f
            if (GuardHit(kGuardFootPlant))
                TO1_LOG("[GUARD] puppet foot-plant IK weight forced to 0\n");
            return out;
        }
        return g_hkNpcFootPlantParams.fastcall<uintptr_t>(out, npc);
    }

    //  ENGINE VALIDITY GUARDS
    static SafetyHookInline g_hkApproxPathUsesTap;

    static __int64 __fastcall hk_ApproxPathUsesTap(uintptr_t mgr, int16_t* handle,
        uintptr_t navHandle, uint8_t* outUsesTap)
    {
        bool bad = (!mgr || !handle);
        if (!bad) {
            __try {
                const uintptr_t mask = *reinterpret_cast<uintptr_t*>(mgr + 0x20);
                if (mask < 0x10000ull || mask > 0x7FFFFFFFFFFFull) bad = true;
                else if (*handle < 0 && static_cast<uint16_t>(*handle) != 0xFFFF) bad = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { bad = true; }
        }

        if (bad) {
            if (GuardHit(kGuardPathTap, 300))
                TO1_LOG("[GUARD] blocked ApproxPathUsesTap on a bad manager/handle\n");
            return 0;
        }
        return g_hkApproxPathUsesTap.fastcall<__int64>(mgr, handle, navHandle, outUsesTap);
    }

    static SafetyHookInline g_hkCompositeBindPhysFx;

    static void __fastcall hk_CompositeBindPhysFx(uintptr_t composite)
    {
        if (!composite) return;

        const uint32_t  count = *reinterpret_cast<uint32_t*>(composite + 20);    // numBodies
        const uint32_t  cap = *reinterpret_cast<uint32_t*>(composite + 24);    // maxBodies
        const uintptr_t records = *reinterpret_cast<uintptr_t*>(composite + 216);
        const uintptr_t bodies = *reinterpret_cast<uintptr_t*>(composite + 96);
        const uintptr_t joints = *reinterpret_cast<uintptr_t*>(composite + 224);

        // joints == 0 already makes the whole loop body a no-op (0x141E0796C).
        if (count && joints) {
            const char* why = nullptr;
            uint32_t    badIndex = 0;

            if (!records)         why = "record array is null";
            else if (!bodies)     why = "body array is null";
            else if (count > cap) why = "numBodies exceeds maxBodies";
            else {
                for (uint32_t i = 0; i < count; ++i) {
                    const uint32_t r = *reinterpret_cast<uint16_t*>(bodies + 272ull * i + 134);
                    if (r >= cap) { why = "recordIndex out of range"; badIndex = r; break; }
                }
            }

            if (why) {
                if (GuardHit(kGuardComposite, 100))
                    TO1_LOG("[PHYSFX] composite %p rejected: %s "
                        "(n=%u cap=%u records=%p bodies=%p owner=%p idx=%u)\n",
                        reinterpret_cast<void*>(composite), why, count, cap,
                        reinterpret_cast<void*>(records), reinterpret_cast<void*>(bodies),
                        *reinterpret_cast<void**>(composite + 8), badIndex);
                return;
            }
        }

        g_hkCompositeBindPhysFx.unsafe_fastcall<void>(composite);
    }

    static SafetyHookInline g_hkHandleResolve;

    static uintptr_t __fastcall hk_HandleResolve(const uint32_t* handle)
    {
        if (handle && *handle > 0x1FFFu) {
            if (GuardHit(kGuardHandle, 500))
                TO1_LOG("[GUARD] rejected out-of-range rigid-body handle index %u\n", *handle);
            return 0;
        }
        return g_hkHandleResolve.fastcall<uintptr_t>(handle);
    }

    static SafetyHookInline g_hkMeleeEventScript;
    static char __fastcall hk_MeleeEventScript(uintptr_t action, uint32_t a2, uintptr_t evt, float a4)
    {
        if (action) {
            using GetAttackerFn = uintptr_t(__fastcall*)(uintptr_t);
            const auto getAttacker = reinterpret_cast<GetAttackerFn>(g_base + 0xEDC870);
            const uintptr_t attacker = getAttacker(action);
            if (NetPlayer* atk = attacker ? RemoteByPtr(attacker) : nullptr) {
                if (!atk->EngineOwned()) {
                    static bool logged = false;
                    if (!logged) {
                        logged = true;
                        TO1_LOG("[MELEE] melee-event script denied: attacker is a puppet\n");
                    }
                    return 0;
                }
            }
        }

        return g_hkMeleeEventScript.fastcall<char>(action, a2, evt, a4);
    }

    void EnsureStealthDenyHook()
    {
        static bool tried = false;
        if (tried || g_hkStealthAvail) return;

        const uintptr_t player = NetPlayers::LocalPtr();
        if (!player) return;
        const uintptr_t ctrl = *reinterpret_cast<uintptr_t*>(player + 0x3108);
        if (!ctrl) return;
        const uintptr_t vt = *reinterpret_cast<uintptr_t*>(ctrl);
        if (!vt) return;
        void* fn = *reinterpret_cast<void**>(vt + 512);
        if (!fn) return;

        tried = true;
        g_hkStealthAvail = safetyhook::create_inline(fn, reinterpret_cast<void*>(hk_StealthAvail));
        TO1_LOG("[MELEE] stealth-availability hook at %p -> %s\n", fn,
            g_hkStealthAvail ? "installed" : "FAILED");
    }

    static SafetyHookInline g_hkAttackDispatch;
    static __int64 __fastcall hk_AttackDispatch(uintptr_t attackInfo, uintptr_t victim)
    {
        if (g_deathBlowVictim && victim == g_deathBlowVictim)
            return g_hkAttackDispatch.fastcall<__int64>(attackInfo, victim);

        if (attackInfo && victim && net::session::IsConnected()) {
            const uint32_t kind = *reinterpret_cast<const uint32_t*>(attackInfo + 0x18);
            if (kind == gameplay::melee::tuning::kAttackKindMelee) {
                const uintptr_t local = NetPlayers::LocalPtr();
                const uintptr_t attacker = AttackerFromAttackInfo(attackInfo);

                float* pDamage = reinterpret_cast<float*>(attackInfo + 0x40);
                const float realDamage = *pDamage;

                NetPlayer* victimPuppet = RemoteByPtr(victim);
                if (victimPuppet && victimPuppet->EngineReleasing())
                    victimPuppet = nullptr;   // released: the move own damage must land
                if (NetPlayer* puppet = victimPuppet) {
                    TO1_LOG("[MELEE] swing landed on puppet \"%s\" (attacker=%llx local=%llx engineDmg=%.1f)\n",
                        puppet->name, static_cast<unsigned long long>(attacker),
                        static_cast<unsigned long long>(local), realDamage);

                    if (local && attacker == local) {
                        if (NetPlayers::SlotIsFriendly(puppet->netSlot))
                            return 0;

                        // The finisher deals its own (instantly lethal) damage as it plays.
                        // Reporting that as an ordinary hit would kill the victim mid-animation,
                        // so swallow every hit the execution itself lands. The kill comes back
                        // as the victim's own confirmed death once their half of the move ends.
                        if (gameplay::melee::ExecutionTargetSlot() == puppet->netSlot) {
                            TO1_LOG("[MELEE] execution damage on \"%s\" swallowed (%.1f)\n",
                                puppet->name, realDamage);
                        }
                        else {
                            uint64_t weapon = net::engine::GetPlayerWeaponId(local);
                            if (!weapon) weapon = *reinterpret_cast<uint64_t*>(attackInfo + 32);

                            const bool engineFinisher =
                                realDamage > gameplay::melee::tuning::kMeleeDamageMax;

                            float damage;
                            if (engineFinisher) {
                                const float ownerHp = puppet->State().health;
                                damage = (ownerHp > 0.0f && ownerHp < 1.0e6f) ? ownerHp : 100.0f;
                            }
                            else {
                                damage = gameplay::melee::ResolveMeleeDamage(realDamage);
                            }

                            const bool wouldBeLethal = engineFinisher ||
                                puppet->ledger.WouldBeLethal(damage, puppet->State().health);

                            if (wouldBeLethal) {
                                const bool claimed =
                                    gameplay::melee::QueueExecution(*puppet, weapon, damage);
                                if (!claimed) {
                                    TO1_LOG("[MELEE] lethal swing on \"%s\" dropped -- no claim available\n",
                                        puppet->name);
                                }
                                gameplay::melee::PlayLocalHitFeedback(*puppet, true);
                            }
                            else {
                                const bool lethal =
                                    ReportLocalHitOnPuppet(*puppet, weapon, 0, damage, false);
                                gameplay::melee::PlayLocalHitFeedback(*puppet, lethal);
                            }
                        }
                    }

                    if (gameplay::melee::tuning::kSuppressLocalPuppetMeleeDamage)
                        *pDamage = 0.0f;
                    const __int64 r = g_hkAttackDispatch.fastcall<__int64>(attackInfo, victim);
                    *pDamage = realDamage;
                    return r;
                }

                if (NetPlayer* atk = (local && victim == local && attacker != local)
                    ? RemoteByPtr(attacker) : nullptr) {
                    if (gameplay::melee::BeingExecutedBy(atk->netSlot)) {
                        TO1_LOG("[MELEE] finisher damage from slot%d allowed through (%.1f)\n",
                            atk->netSlot, realDamage);
                    }
                    else {
                        TO1_LOG("[MELEE] replayed swing landed on us (engineDmg=%.1f)%s\n", realDamage,
                            gameplay::melee::tuning::kSuppressReplayedSwingDamage ? " -> suppressed" : "");
                        if (gameplay::melee::tuning::kSuppressReplayedSwingDamage)
                            return 0;
                    }
                }
            }
        }
        return g_hkAttackDispatch.fastcall<__int64>(attackInfo, victim);
    }

    // skip death sequence, only play death state

    SafetyHookInline gDeathHook;
    __int64 __fastcall hk_PlayerDeath(__int64 player) {

        if (uintptr_t hc = *(uintptr_t*)(player + 1552))                       // health notify
            ((void(__fastcall*)(uintptr_t))(*(uintptr_t*)(*(uintptr_t*)hc + 48)))(hc);

        auto getCtrl = (uintptr_t(__fastcall*)(uintptr_t))(*(uintptr_t*)(*(uintptr_t*)player + 4480));
        if (getCtrl) if (uintptr_t c = getCtrl(player))                        // +4480 notify
            ((void(__fastcall*)(uintptr_t))(*(uintptr_t*)(*(uintptr_t*)c + 48)))(c);

        net::engine::RequestState(player, g_base + 0x35BD410);                                // enter Death = animation
        return 0;
    }


    void ForceLocalDeath(const char* why)
    {
        if (!net::session::IsConnected()) return;
        const uintptr_t player = NetPlayers::LocalPtr();
        if (!player) return;
        if (PLAYER::IS_IN_STATE(SID("Death"))) return;   // the engine already got there

        TO1_LOG("[MELEE] forcing local death (%s) -- the finisher never landed its own kill\n",
            why ? why : "?");

        // Exactly what hk_PlayerDeath does: the two notifies, then the Death state.
        if (uintptr_t hc = *(uintptr_t*)(player + 1552))
            ((void(__fastcall*)(uintptr_t))(*(uintptr_t*)(*(uintptr_t*)hc + 48)))(hc);
        auto getCtrl = (uintptr_t(__fastcall*)(uintptr_t))(*(uintptr_t*)(*(uintptr_t*)player + 4480));
        if (getCtrl) if (uintptr_t c = getCtrl(player))
            ((void(__fastcall*)(uintptr_t))(*(uintptr_t*)(*(uintptr_t*)c + 48)))(c);
        net::engine::RequestState(player, g_base + 0x35BD410);
    }

    SafetyHookInline g_hkDeathStateUpdate;
    typedef __int64(__fastcall* tBaseStateUpdate)(__int64);
    __int64 __fastcall hk_DeathStateUpdate(__int64 a1)
    {
        static auto baseUpdate = (tBaseStateUpdate)(
            g_base + 0xAD9540
            );
        return baseUpdate(a1);
    }

    static bool g_installed = false;
    static uint8_t g_overlayOrig[4];
    static bool    g_overlaySaved = false;

    // outdated, we dont use player puppets anymore so not really needed
    SafetyHookInline g_hkDestroyHudRoot;
    __int64 __fastcall hk_DestroyHudRoot()
    {
        if (NetPlayers::LocalPtr())
            return 0;
        return g_hkDestroyHudRoot.fastcall<__int64>();
    }

    static SafetyHookInline g_hkCraftPush;
    static SafetyHookInline g_hkCraftExit;
    static volatile long    g_dCraftPage = 0;
    static bool             g_backpackPageUp = false;

    // was winmm_patches.py: tab-native-backpack (EXPERIMENT, default off). When true the mod's
    // backpack store detour is not installed, so TAB opens the game's own backpack/crafting menu.
    static constexpr bool kTabNativeBackpack = false;

    static uintptr_t __fastcall hk_CraftPush(uintptr_t hud2, uintptr_t a2, int tab)
    {
        (void)hud2; (void)a2; (void)tab;
        if (!g_backpackPageUp) {
            MENU2::PUSH_PAGE(MenuStack::hudMenuStack, "to1/backpack-menu");
            g_backpackPageUp = true;
            const long n = _InterlockedIncrement(&g_dCraftPage);
            TO1_LOG("[backpack] default menu suppressed -> pushed to1/backpack-menu (%ld)\n", n);
        }
        return 0;
    }

    static uintptr_t __fastcall hk_CraftExit(uintptr_t a1)
    {
        const uintptr_t r = g_hkCraftExit.fastcall<uintptr_t>(a1);
        if (g_backpackPageUp) {
            DC::Call<"menu2-pop-page">(SID("hudMenuStack"), SID("to1/backpack-menu"));
            g_backpackPageUp = false;
            TO1_LOG("[backpack] popped to1/backpack-menu\n");
        }
        return r;
    }

    static SafetyHookMid g_hkWeapon_EmitProjectile;
    static void hkWeapon_EmitProjectile(SafetyHookContext& ctx)
    {
        const uintptr_t weapon = ctx.rcx;
        const uintptr_t owner = ctx.rdx;
        if (weapon < 0x10000 || !net::session::IsConnected()) return;

        NetPlayer* shooter = RemoteByPtr(owner);
        if (!shooter) {
            for (NetPlayer& p : NetPlayers::Remotes()) {
                if (!p.pendingLaunchValid) continue;
                if (net::engine::GetHeldFirearm(p.Ptr()) == weapon) { shooter = &p; break; }
            }
        }
        if (!shooter || !shooter->pendingLaunchValid) return;

        const float age = net::replication::NowSec() - shooter->pendingLaunchAt;
        shooter->pendingLaunchValid = false;
        if (age < 0.0f || age > kPendingLaunchTtlSec) {
            TO1_LOG("[FIRE] dropped stale launch velocity (age %.2fs) for %s\n",
                age, shooter->DisplayName());
            return;
        }

        if (net::engine::ArmLaunchVelocity(weapon, shooter->pendingLaunchVel))
            TO1_LOG("[FIRE] armed %.2f m/s on %s puppet arrow\n",
                Length3(shooter->pendingLaunchVel), shooter->DisplayName());
    }

    static SafetyHookInline g_hkSpawnProjectile;
    static void __fastcall hk_SpawnProjectile(uintptr_t weapon, uintptr_t spawnInfo, uintptr_t* outProcess)
    {
        g_hkSpawnProjectile.fastcall<void>(weapon, spawnInfo, outProcess);
        if (!weapon || !outProcess || !*outProcess) return;
        const uintptr_t local = NetPlayers::LocalPtr();
        if (local && *reinterpret_cast<uint32_t*>(weapon + 0xDB0 + 0x48) ==
            *reinterpret_cast<uint32_t*>(local + 0x94))
            gameplay::barrelcheck::LatchProjectile(*outProcess);
    }

    static SafetyHookInline g_hkVoxMute;
    static bool __fastcall hk_VoxDeadMute(unsigned int playerIdx)
    {
        if (net::session::IsRunning())
            return false;
        return g_hkVoxMute.fastcall<bool>(playerIdx);
    }

    namespace shoulder {
        enum : uintptr_t {
            RVA_GetHeldWeapon = 0x0BDC1F0,  // (itemCtrl) -> ProcessWeaponBase*
            RVA_WeaponAnimType = 0x1163A40,  // (weaponProc) -> art anim type
            RVA_IsMirrored = 0x0DAB150,  // (player) -> animCtrl[0x17D] == 1
            RVA_SetMirrored = 0x0DC0360,  // (player, v) -> animCtrl[0x17D]
            RVA_SetMirrorCopies = 0x0D90800,  // (player, v) -> animData[590], animCtrl[768]
            RVA_AllocParamSlot = 0x1257740,
            RVA_SetAnimParam = 0x202A910,
            RVA_GetArmIndex = 0x1161390,  // (weaponSid, obj) -> nonzero = right hand
            RVA_SwitchItemHand = 0x0BDEB90,  // InstantSwitchItemHand(itemCtrl, armIndex)
            RVA_GetAttachPoint = 0x1CD4870,  // (obj, &outSid)
            RVA_StrafeShoulderOffset = 0x0816EA0,  // CameraControlStrafe shoulder-offset target
        };
        enum : uintptr_t {
            OFF_WEAPON_CTRL = 0x2F60, // player -> PlayerItemController
            OFF_AIM_CTRL = 0x2F68, // player -> PlayerAimController
            AIM_SCOPE_ZOOM = 0x0698, // PlayerAimController::m_scopeZoomedIn
            OFF_GUN_SIDE = 0x4820,
            OFF_ANIM_PARAMS = 0x50B8,
            OFF_WEAPON_SID = 0x840, // weaponProc -> art def SID
        };
        enum : uintptr_t {
            IC_OWNER = 0x68, IC_HANDLE = 0x70, IC_ID = 0x78,
            IC_ARM = 0x80, IC_MIRROR = 0x84, IC_VALID = 0x88,
        };
        static const StringId64 leftHand = SID("leftHand");
        static const StringId64 rightHand = SID("rightHand");

        template <typename T> static T Fn(uintptr_t rva) { return reinterpret_cast<T>(g_base + rva); }

        volatile long g_pistolHandFix = 1;
        volatile long g_hipCameraFollowsSwap = 1;
        volatile long g_camSwapped = 0;

        static void PushParam(uintptr_t blob, uint64_t sid, uint8_t value)
        {
            uint8_t* slot = Fn<uint8_t * (__fastcall*)(uintptr_t)>(RVA_AllocParamSlot)(blob + 184);
            uint64_t t = *reinterpret_cast<uint64_t*>(blob + 248);
            if (*reinterpret_cast<uint64_t*>(blob + 16) > t) t = *reinterpret_cast<uint64_t*>(blob + 16);
            *reinterpret_cast<uint64_t*>(blob + 16) = t;
            if (slot) *slot = value;
            Fn<void(__fastcall*)(uintptr_t, uint64_t, void*)>(RVA_SetAnimParam)(blob, sid, slot);
        }

        static void ApplyMirror(uintptr_t player, bool mirror)
        {
            Fn<void(__fastcall*)(uintptr_t, char)>(RVA_SetMirrored)(player, static_cast<char>(mirror));
            Fn<void(__fastcall*)(uintptr_t, char)>(RVA_SetMirrorCopies)(player, static_cast<char>(mirror));
            if (const uintptr_t blob = *reinterpret_cast<uintptr_t*>(player + OFF_ANIM_PARAMS)) {
                PushParam(blob, SID("MotionMatchMirror"), static_cast<uint8_t>(mirror));
                PushParam(blob, SID("MotionMatchForceRefresh"), 1);
            }
        }

        static int       g_lastWantArm = -1;
        static int       g_lastCommanded = -1;
        static uintptr_t g_lastWeapon = 0;

        static void ResetPistolHandState()
        {
            g_lastWantArm = -1; g_lastCommanded = -1; g_lastWeapon = 0;
        }

        static void ReassertPistolHand(uintptr_t player, bool mirrored)
        {
            if (!g_pistolHandFix) return;

            const uintptr_t ic = *reinterpret_cast<uintptr_t*>(player + OFF_WEAPON_CTRL);
            if (!ic || !*reinterpret_cast<uintptr_t*>(ic + IC_OWNER)) return;

            const uintptr_t weapon = Fn<uintptr_t(__fastcall*)(uintptr_t)>(RVA_GetHeldWeapon)(ic);
            if (!weapon) return;
            if (Fn<uint32_t(__fastcall*)(uintptr_t)>(RVA_WeaponAnimType)(weapon) != 1) return; // pistols only

            const uintptr_t hA = *reinterpret_cast<uintptr_t*>(ic + IC_HANDLE);
            const uint32_t  idA = *reinterpret_cast<uint32_t*>(ic + IC_ID);
            if (!hA || !idA || *reinterpret_cast<int32_t*>(ic + IC_VALID) < 0) return;
            const uintptr_t procA = *reinterpret_cast<uintptr_t*>(hA + 8);
            if (!procA || *reinterpret_cast<uint32_t*>(procA + 148) != idA) return;

            const uint64_t wsid = *reinterpret_cast<uint64_t*>(weapon + OFF_WEAPON_SID);
            const bool     baseRight = Fn<bool(__fastcall*)(uint64_t, uintptr_t)>(RVA_GetArmIndex)(wsid, 0);
            const uint32_t wantArm = static_cast<uint32_t>(mirrored ? !baseRight : baseRight);

            uint64_t cur = 0;
            Fn<uint64_t* (__fastcall*)(uintptr_t, uint64_t*)>(RVA_GetAttachPoint)(weapon, &cur);
            if (cur != leftHand && cur != rightHand) return;
            const uint32_t curArm = (cur == rightHand) ? 1u : 0u;

            const bool weaponChanged = (weapon != g_lastWeapon);
            const bool wantChanged = (g_lastWantArm != static_cast<int>(wantArm));
            const bool weOwnIt = (g_lastCommanded >= 0 &&
                curArm == static_cast<uint32_t>(g_lastCommanded));
            g_lastWeapon = weapon;
            g_lastWantArm = static_cast<int>(wantArm);

            if (curArm == wantArm) {
                g_lastCommanded = static_cast<int>(wantArm);
                return;
            }
            if (!weaponChanged && !wantChanged && !weOwnIt) {
                g_lastCommanded = -1;
                return;
            }

            *reinterpret_cast<uint32_t*>(ic + IC_ARM) =
                static_cast<uint32_t>(curArm != *reinterpret_cast<uint8_t*>(ic + IC_MIRROR));
            Fn<void(__fastcall*)(uintptr_t, uint32_t)>(RVA_SwitchItemHand)(ic, wantArm);
            g_lastCommanded = static_cast<int>(wantArm);
            TO1_LOG("[SHOULDER] pistol -> %s hand\n", wantArm ? "right" : "left");
        }

        static uintptr_t HeldWeapon(uintptr_t player)
        {
            const uintptr_t ic = *reinterpret_cast<uintptr_t*>(player + OFF_WEAPON_CTRL);
            return ic ? Fn<uintptr_t(__fastcall*)(uintptr_t)>(RVA_GetHeldWeapon)(ic) : 0;
        }
    }

    // hip camera shoulder
    static SafetyHookInline g_hkStrafeShoulderOffset;
    static float __fastcall hk_StrafeShoulderOffset(uintptr_t cam, const void* vec)
    {
        const float v = g_hkStrafeShoulderOffset.fastcall<float>(cam, vec);
        if (shoulder::g_hipCameraFollowsSwap && shoulder::g_camSwapped)
            return -v;
        return v;
    }

    static SafetyHookInline g_hkTryShoulderSwap;

    static char __fastcall hk_TryShoulderSwap(uintptr_t player)
    {
        uint8_t* aim = nullptr;
        uint8_t  saved = 0;
        if (net::session::IsRunning() && player) {
            if (const uintptr_t ac = *reinterpret_cast<uintptr_t*>(player + shoulder::OFF_AIM_CTRL)) {
                aim = reinterpret_cast<uint8_t*>(ac + shoulder::AIM_SCOPE_ZOOM);
                saved = *aim;
                *aim = 0;
            }
        }

        const char r = g_hkTryShoulderSwap.fastcall<char>(player);

        if (aim) *aim = saved;

        if (!net::session::IsRunning() || !player) {
            shoulder::g_camSwapped = 0;
            if (player && shoulder::g_lastCommanded >= 0)
                shoulder::ReassertPistolHand(player, false);
            return r;
        }

        const uintptr_t weapon = shoulder::HeldWeapon(player);
        const uint32_t  gunSide = *reinterpret_cast<const uint32_t*>(player + shoulder::OFF_GUN_SIDE);
        const uint32_t  animTy = weapon
            ? shoulder::Fn<uint32_t(__fastcall*)(uintptr_t)>(shoulder::RVA_WeaponAnimType)(weapon) : 0;
        const bool want = (gunSide == 0) && (weapon == 0 || animTy != 8);

        // One source of truth for all three followers: mirror, pistol hand, hip camera.
        shoulder::g_camSwapped = (gunSide == 0) ? 1 : 0;

        if (shoulder::Fn<bool(__fastcall*)(uintptr_t)>(shoulder::RVA_IsMirrored)(player) != want)
            shoulder::ApplyMirror(player, want);

        shoulder::ReassertPistolHand(player, want);
        return r;
    }

    void InstallMultiplayerHooks()
    {
        if (g_installed) return;
        g_installed = true;

        const uint8_t kOverlayRet[] = { 0x48, 0x89, 0xC8, 0xC3 };
        if (!g_overlaySaved) {
            memcpy(g_overlayOrig, reinterpret_cast<void*>(g_base + 0x040AFE0), sizeof(g_overlayOrig));
            g_overlaySaved = true;
        }
        WriteBytes(g_base + 0x040AFE0, kOverlayRet, sizeof(kOverlayRet));

        g_hkShot = safetyhook::create_mid(reinterpret_cast<void*>(g_base + 0x1182D5B), hk_Shot);
        g_hkWeapon_EmitProjectile = safetyhook::create_mid(reinterpret_cast<void*>(g_base + 0x118CEF0), hkWeapon_EmitProjectile);
        g_hkSpawnProjectile = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x118F920), reinterpret_cast<void*>(hk_SpawnProjectile));
        g_dFireCue = 0;
        g_hkFireCue = safetyhook::create_mid(reinterpret_cast<void*>(g_base + 0x1184462), hk_FireCue);
        g_hkProjectileThrow = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x0DC38E0), reinterpret_cast<void*>(hk_ProjectileThrow));
        g_hkDoorOpen = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0xB63250), reinterpret_cast<void*>(hk_DoorOpen));
        g_hkShouldSprint = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0xCD3D50), reinterpret_cast<void*>(hk_ShouldSprint));
        g_hkBulletHit = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x11A0280), reinterpret_cast<void*>(hk_BulletHitVictim));
        g_hkCompositeMotionApply = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x1E0D600), reinterpret_cast<void*>(hk_CompositeMotionApply));
        g_hkAnimLayerCurInst = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x1601830), reinterpret_cast<void*>(hk_AnimLayerCurInst));
        g_dJointOob = 0;
        g_hkJointLocator = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x1353B80), reinterpret_cast<void*>(hk_JointLocator));
        g_hkJointEval = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x1353420), reinterpret_cast<void*>(hk_JointEval));
        g_hkTryAttack = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0xECA210), reinterpret_cast<void*>(hk_TryAttack));
        g_hkMeleeTargetFilter = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0xEBED00), reinterpret_cast<void*>(hk_MeleeTargetFilter));
        g_hkMeleeEventScript = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0xEF8FE0), reinterpret_cast<void*>(hk_MeleeEventScript));
        g_hkFindMeleeMove = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0xE41B10), reinterpret_cast<void*>(hk_FindMeleeMove));
        if (!g_hkFindMeleeMove)
            TO1_LOG("[HOOKS][BUG] FindMeleeMove hook install FAILED\n");

        // ---- puppet guards + engine validity guards (see the PUPPET GUARDS section) ----
        for (int i = 0; i < kGuardCount; ++i) g_guardHits[i] = 0;
        g_hkUpdateRootLocator = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x1D59650), reinterpret_cast<void*>(hk_UpdateRootLocator));
        g_hkTeleportToBoundFrame = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x1D58840), reinterpret_cast<void*>(hk_TeleportToBoundFrame));
        g_hkNpcLegIkMode = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x6A3170), reinterpret_cast<void*>(hk_NpcLegIkMode));
        g_hkNpcFootPlantParams = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x4FC3A0), reinterpret_cast<void*>(hk_NpcFootPlantParams));
        g_hkApproxPathUsesTap = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x1CDE8F0), reinterpret_cast<void*>(hk_ApproxPathUsesTap));
        g_hkCompositeBindPhysFx = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x1E078E0), reinterpret_cast<void*>(hk_CompositeBindPhysFx));
        g_hkHandleResolve = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x15F9980), reinterpret_cast<void*>(hk_HandleResolve));
        g_hkAttackDispatch = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x395360), reinterpret_cast<void*>(hk_AttackDispatch));
        g_hkMeleeDamageRadius = safetyhook::create_inline(reinterpret_cast<void*>(g_base + gameplay::melee::tuning::kRvaDamageTargetInRadius), reinterpret_cast<void*>(hk_MeleeDamageTargetInRadius));
        g_hkHealthApplyDamage = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x1FEB9A0), reinterpret_cast<void*>(hk_HealthApplyDamage));

        if (!g_hkTryAttack || !g_hkMeleeTargetFilter || !g_hkMeleeEventScript ||
            !g_hkAttackDispatch || !g_hkMeleeDamageRadius || !g_hkHealthApplyDamage)
            TO1_LOG("[HOOKS][BUG] melee hook install FAILED: tryAtk=%d filter=%d evtScript=%d dispatch=%d dmgRadius=%d health=%d\n",
                g_hkTryAttack ? 1 : 0, g_hkMeleeTargetFilter ? 1 : 0, g_hkMeleeEventScript ? 1 : 0,
                g_hkAttackDispatch ? 1 : 0, g_hkMeleeDamageRadius ? 1 : 0, g_hkHealthApplyDamage ? 1 : 0);

        // skip death sequence
        g_hkDeathStateUpdate = safetyhook::create_inline((void*)(g_base + 0xB5D010), hk_DeathStateUpdate);
        gDeathHook = safetyhook::create_inline((void*)(g_base + 0xB59DD0), hk_PlayerDeath);

        // stops engine from hiding npc/actor/cloths on local death
        g_hkHideUninvolved = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x0B5DF40), reinterpret_cast<void*>(hk_HideUninvolvedOnDeath));

        // fix to prevent vox on other characters being muted on local player death
        g_hkVoxMute = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x0DB9960), reinterpret_cast<void*>(hk_VoxDeadMute));

        // custom crafting menu (was winmm_patches.py: tab-native-backpack -- skipped when
        // kTabNativeBackpack, leaving the game's native backpack/crafting menu on TAB)
        g_backpackPageUp = false;
        g_dCraftPage = 0;
        if (!kTabNativeBackpack) {
            g_hkCraftPush = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x0F7B740), reinterpret_cast<void*>(hk_CraftPush));
            g_hkCraftExit = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x0BBD630), reinterpret_cast<void*>(hk_CraftExit));
        }

        // remove
        g_hkDestroyHudRoot = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x1F21570), reinterpret_cast<void*>(hk_DestroyHudRoot));

        // new shoulder swap
        shoulder::ResetPistolHandState();
        shoulder::g_camSwapped = 0;
        g_hkTryShoulderSwap = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0xAD3E80), reinterpret_cast<void*>(hk_TryShoulderSwap));
        g_hkStrafeShoulderOffset = safetyhook::create_inline(reinterpret_cast<void*>(g_base + shoulder::RVA_StrafeShoulderOffset), reinterpret_cast<void*>(hk_StrafeShoulderOffset));

        TO1_LOG("[HOOKS] multiplayer hook set installed\n");
    }

    void RemoveMultiplayerHooks()
    {
        if (!g_installed) return;

        g_hkShot.reset();
        g_hkFireCue.reset();
        g_hkWeapon_EmitProjectile.reset();
        g_hkSpawnProjectile.reset();
        g_hkProjectileThrow.reset();
        g_hkDoorOpen.reset();
        g_hkShouldSprint.reset();
        g_hkBulletHit.reset();
        g_hkCompositeMotionApply.reset();
        g_hkAnimLayerCurInst.reset();
        g_hkJointLocator.reset();
        g_hkJointEval.reset();

        g_hkTryAttack.reset();
        g_hkAttackDispatch.reset();
        g_hkMeleeDamageRadius.reset();

        g_hkMeleeTargetFilter.reset();
        g_hkMeleeEventScript.reset();
        g_hkFindMeleeMove.reset();
        g_hkApproxPathUsesTap.reset();
        g_hkUpdateRootLocator.reset();
        g_hkTeleportToBoundFrame.reset();
        g_hkHandleResolve.reset();
        g_hkCompositeBindPhysFx.reset();
        g_hkNpcLegIkMode.reset();
        g_hkNpcFootPlantParams.reset();
        g_hkHealthApplyDamage.reset();

        g_hkDeathStateUpdate.reset();
        gDeathHook.reset();
        g_hkDestroyHudRoot.reset();

        g_hkCraftPush.reset();
        g_hkCraftExit.reset();
        g_backpackPageUp = false;

        g_hkHideUninvolved.reset();
        g_hkVoxMute.reset();

        g_hkTryShoulderSwap.reset();
        g_hkStrafeShoulderOffset.reset();
        shoulder::ResetPistolHandState();
        shoulder::g_camSwapped = 0;

        if (g_overlaySaved)
            WriteBytes(g_base + 0x040AFE0, g_overlayOrig, sizeof(g_overlayOrig));

        g_installed = false;
        TO1_LOG("[HOOKS] multiplayer hook set removed\n");
    }

}
