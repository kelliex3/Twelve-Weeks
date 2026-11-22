// ported integration seams for the pure melee cores (see melee-bridge.hpp)

#include "melee-bridge.hpp"

#include "engine/frame.hpp"
#include "engine/anim-prims.hpp"
#include "engine/pad.hpp"
#include "engine/ptr.hpp"
#include "engine/script.hpp"
#include "log.hpp"
#include "net/online/net-session.hpp"
#include "net/protocol/net-protocol.hpp"
#include "net/game/net-engine.hpp"
#include "net/game/net-replication.hpp"
#include "net/match/hit-rel.hpp"
#include "net/match/match-liveness.hpp"
#include "net/match/net-player.hpp"
#include "gameplay/melee-dodge.hpp"
#include "gameplay/melee-duel.hpp"
#include "gameplay/melee-hit-react.hpp"
#include "gameplay/melee-exchange.hpp"
#include "gameplay/melee-tell.hpp"
#include "sid/sid.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace net::match::melee_bridge {

    namespace {

        uint64_t ClockMs()
        {
            const double ms = ::engine::frame::NowMs();
            return ms > 0.0 ? static_cast<uint64_t>(ms) : 0ull;
        }

        void Log(const char* text)
        {
            if (text)
                TO1_LOG("[melee-bridge] %s", text);
        }

        uint8_t LocalSlot8()
        {
            const int s = net::session::MySlot();
            return s < 0 ? 255u : static_cast<uint8_t>(s);
        }

        // Every reserved melee payload goes out through the same event channel the low opcodes
        // use; SendEventRaw also feeds net-direct's cross-path dedupe / low-latency copy.
        void Send(NetEventType type, const void* body, uint8_t len,
                  uint8_t dest = kBroadcastSlot)
        {
            net::session::SendEventRaw(static_cast<uint8_t>(type), body, len, dest);
        }

        // The reliable-hit payload is the mod's PlayerDamageEvent. Re-entering HandleNetEvent
        // with the plain opcode runs the full receive path (hit bookkeeping, stings, music
        // inputs, lethal claim and FireAttack) instead of duplicating it here. TAKEDAMAGE is
        // not one of the reserved opcodes, so this does not recurse into melee_bridge::OnEvent.
        void ApplyHitPayload(uint8_t from, const uint8_t hit[0x16])
        {
            if (from < kMaxNetPlayers)
                net::replication::HandleNetEvent(from, static_cast<uint8_t>(NEVT_TAKE_DAMAGE),
                                                 hit, 0x16);
        }

        // Puppet reaction clips are keyed by sid (the ext's MhrAnimate). anim::AnimateFrom only
        // takes a name, so write the same two-entry script array here and call animate_ directly.
        uint64_t AnimateSid(uint64_t entity, uint64_t sid, float blend, float phase)
        {
            if (!entity || !sid)
                return ~0ULL;
            const uint64_t arr = MEMORY::ALLOC_ARRAY(16, 2, 1);
            if (!arr || arr == ~0ULL || !::engine::ptr::Readable(static_cast<uintptr_t>(arr), 8))
                return ~0ULL;
            uint64_t* tab = *reinterpret_cast<uint64_t**>(static_cast<uintptr_t>(arr));
            if (!tab || !::engine::ptr::Readable(reinterpret_cast<uintptr_t>(tab), 16))
                return ~0ULL;
            uint8_t* e0 = reinterpret_cast<uint8_t*>(tab[0]);
            uint8_t* e1 = reinterpret_cast<uint8_t*>(tab[1]);
            if (!e0 || !e1
                || ::engine::anim::AllocEntryBad(reinterpret_cast<void*>(static_cast<uintptr_t>(arr)), e0, 16)
                || ::engine::anim::AllocEntryBad(reinterpret_cast<void*>(static_cast<uintptr_t>(arr)), e1, 16))
                return ~0ULL;
            const int32_t kBlend = 28, kPhase = 58;
            std::memcpy(e0, &kBlend, 4);
            std::memcpy(e0 + 8, &blend, 4);
            std::memcpy(e1, &kPhase, 4);
            std::memcpy(e1 + 8, &phase, 4);
            return DC::Call<"animate_">(entity, sid, arr);
        }

        uint64_t BodyOf(uint8_t slot)
        {
            if (slot == 255)
                return SID("player");
            if (slot >= kMaxNetPlayers)
                return 0;
            return NetPlayers::BySlot(slot).npcSid;
        }

        // The ext's MhrStop: stop-animating(entity, ANIM_LAYER, blend, sid, 0). A blend of
        // exactly 0 drops the layer state (the ext's crash note), so floor it at 0.10 s.
        void StopSid(uint64_t entity, uint64_t sid, float blendSec)
        {
            if (!entity || !sid)
                return;
            if (blendSec < 0.10f)
                blendSec = 0.10f;
            DC::Call<"stop-animating">(entity, 2u /* ANIM_LAYER */, blendSec, sid, 0u);
        }

        // ---- hitrel -----------------------------------------------------------------------
        // The reliable body is sent as NEVT_HIT_REL; the non-capable fallbacks reuse the mod's
        // plain event opcodes so a 4-slot mod client and an ext client stay interoperable.
        // Armour / molotov payload decoders are still gated (no native producer for either).
        class HitRelBridge final : public net::match::hitrel::Sink {
        public:
            uint64_t NowMs() override { return ClockMs(); }
            bool InMatch() override { return net::match::liveness::Live(); }
            bool PeerActive(uint8_t slot) override
            {
                return slot < kMaxNetPlayers && static_cast<int>(slot) != net::session::MySlot()
                    && NetPlayers::BySlot(slot).Exists();
            }
            uint8_t LocalSlot() override { return LocalSlot8(); }
            void SendWire(uint8_t dest, const uint8_t* msg, uint8_t len) override
            {
                Send(NEVT_HIT_REL, msg, len, dest);
            }
            void SendPlainHit(uint8_t dest, const uint8_t hit[0x16]) override
            {
                Send(NEVT_TAKE_DAMAGE, hit, 0x16, dest);
            }
            void SendPlainArmor(uint8_t dest, const uint8_t armor[11]) override
            {
                Send(NEVT_ARMOR_HIT, armor, 11, dest);
            }
            void SendPlainBurst(uint8_t, const uint8_t*, uint8_t) override {} // no molotov opcode
            void ApplyHit(uint8_t from, const uint8_t hit[0x16]) override
            {
                ApplyHitPayload(from, hit);
            }
            void ApplyArmor(uint8_t, const uint8_t[11]) override {} // ext armour decoder not ported
            void ApplyBurst(uint8_t, const uint8_t*, uint8_t) override {} // ext burst decoder not ported
            void Log(const char* text) override { melee_bridge::Log(text); }
        };

        // ---- dodge ------------------------------------------------------------------------
        // The three messages are the ext's exact bytes: 0x97 [u8 kind][u8 wcls1][u8 wcls2],
        // 0x99 [u8 immune|heavy][u8 dir][u64 clip sid LE], 0x98 empty. The pure core only tracks
        // the armed bit / immune bit / dir / sid, so the two class bytes stay 0 until a local
        // swing-class producer lands (peers read buf[0] only).
        class DodgeBridge final : public gameplay::melee_dodge::Sink {
        public:
            void SendSwing(uint8_t dest) override
            {
                const uint8_t body[3] = { 0, 0, 0 };
                Send(NEVT_DODGE_SWING, body, sizeof body, dest);
            }
            void SendPress(uint8_t dest, uint8_t immune, uint8_t dir, uint64_t clipSid) override
            {
                uint8_t body[10] = {};
                body[0] = immune;
                body[1] = dir;
                for (int i = 0; i < 8; ++i)
                    body[2 + i] = static_cast<uint8_t>(clipSid >> (8 * i));
                Send(NEVT_DODGE_PRESS, body, sizeof body, dest);
            }
            void SendResult(uint8_t dest) override
            {
                const uint8_t body[1] = { 0 };
                Send(NEVT_DODGE_RES, body, sizeof body, dest);
            }
            void ReplayBlow(const gameplay::melee_dodge::HeldBlow& blow) override
            {
                // Grace ended with no dodge: the held contact goes down the same engine damage
                // path a live hit uses, then the swing is resolved so its window closes.
                if (blow.slot < 0 || blow.slot >= kMaxNetPlayers || !blow.me || !blow.attacker)
                    return;
                net::engine::FireAttack(blow.me, blow.attacker, blow.weapon, blow.ammo,
                                        blow.damage);
                gameplay::melee_dodge::NoteSwingLanded(blow.slot, ClockMs());
            }
            void Log(const char* text) override { melee_bridge::Log(text); }
        };

        // ---- hit reactions -----------------------------------------------------------------
        class HitReactBridge final : public gameplay::melee_hit_react::Sink {
        public:
            uint64_t NowMs() override { return ClockMs(); }
            // The SP clip table (melee_hitreact_rm.h) is private / unported, so we cannot pick a
            // reaction of our own. A peer's 0x9A still carries a real clip sid, which the receive
            // path below animates on its puppet.
            bool PickClip(const gameplay::melee_hit_react::Blow&, int, bool, uint64_t,
                          uint64_t*, uint16_t*, int*) override { return false; }
            // The authored cut is the reaction length; the full clip-length lookup needs the
            // clip name (we only have its sid on the wire), so the cut is the honest bound.
            uint64_t ExitMsOf(uint64_t, uint16_t cutMs) override { return cutMs; }
            void PlayLocalReaction(uint64_t sid, uint16_t, int) override
            {
                AnimateSid(SID("player"), sid, 0.10f, 0.0f);
            }
            void StopLocalReaction(uint64_t sid, float blendSec) override
            {
                StopSid(SID("player"), sid, blendSec);
            }
            void PlayPuppetReaction(uint8_t slot, uint64_t sid, uint16_t, uint64_t) override
            {
                const uint64_t body = BodyOf(slot);
                if (body)
                    AnimateSid(body, sid, 0.10f, 0.0f);
            }
            void StopPuppetReaction(uint8_t slot, uint64_t sid, float blendSec) override
            {
                const uint64_t body = BodyOf(slot);
                if (body)
                    StopSid(body, sid, blendSec);
            }
            void PainSound(uint8_t slot, int strength, bool sharp) override
            {
                static const char* const SH[3] = {
                    "vox-eff-gen-fight-take-sml-shrp", "vox-eff-gen-fight-take-med-shrp",
                    "vox-eff-gen-fight-take-med-shrp" };
                static const char* const BL[3] = {
                    "vox-eff-gen-fight-take-face", "vox-eff-gen-fight-take-face",
                    "vox-eff-gen-fight-take-lrg-blnt" };
                const uint64_t body = BodyOf(slot);
                if (!body)
                    return;
                const int s = std::clamp(strength, 0, 2);
                AUDIO::PLAY_VOICE_LINE(body, SID(sharp ? SH[s] : BL[s]));
            }
            void Punch(uint8_t, int) override {}    // punch-at-joint needs the actor joint cache
            void HitFxConfirm(uint8_t) override {}  // blade / blunt FX confirm needs the contact FX
            bool PredBlocked(uint8_t) override { return true; }  // no dodge / down / busy feed
            float RttMs(uint8_t slot) override
            {
                if (slot >= kMaxNetPlayers)
                    return 0.0f;
                const int ms = NetPlayers::BySlot(slot).PingMs();
                return ms > 0 ? static_cast<float>(ms) : 0.0f;
            }
            void SendMessage(const uint8_t body[gameplay::melee_hit_react::kMessageSize]) override
            {
                Send(NEVT_HITREACT2, body, gameplay::melee_hit_react::kMessageSize);
            }
            void Log(const char* text) override { melee_bridge::Log(text); }
        };

        // ---- exchange ----------------------------------------------------------------------
        class ExchangeBridge final : public gameplay::melee_exchange::Sink {
        public:
            uint64_t NowMs() override { return ClockMs(); }
            uint8_t LocalSlot() override { return LocalSlot8(); }
            float OnewayMs(uint8_t slot) override
            {
                if (slot >= kMaxNetPlayers)
                    return 0.0f;
                const int rtt = NetPlayers::BySlot(slot).PingMs();
                return rtt > 0 ? static_cast<float>(rtt) * 0.5f : 0.0f;
            }
            uint16_t TellTc(int wcls, uint16_t authored) override
            {
                return gameplay::melee_tell::Tc(wcls, authored);
            }
            uint16_t QteFlags() override { return 0; }       // QTE system not ported
            uint16_t QtePressOffset() override { return 0; }
            uint8_t QteCap() override { return 0; }
            bool PredBlocked(uint8_t) override { return true; }  // no busy / down / reach feed
            void Send(uint8_t dest, const uint8_t* body, uint8_t len) override
            {
                melee_bridge::Send(NEVT_MEX, body, len, dest);
            }
            void Log(const char* text) override { melee_bridge::Log(text); }
            void PlayFreeSwing() override { ::engine::pad::Press("melee-attack", 40); }
            void FireSwing() override { ::engine::pad::Press("melee-attack", 40); }
            void ReplayHit(uint8_t) override {} // the engine-held contact replay needs the contact hook
            void PlayMiss() override {}         // the attacker's stumble / momentum needs that hook too
            void BeginQte(uint8_t) override {}  // QTE system not ported
            void EndQte() override {}
        };

        // melee-dodge's window narrowing: the exchange / duel cores are pure and already feed
        // their window ends from their own wire / lifecycle state. The geometry callback is
        // deliberately absent until a native peer-pose producer exists (no engine reads here).
        int ExchangeWindowEnd(int slot, uint64_t* end)
        {
            return gameplay::melee_exchange::WindowEnd(static_cast<uint8_t>(slot), end);
        }

        int DuelWindowEnd(int slot, uint64_t nowMs, uint64_t swingMs, uint64_t* end)
        {
            return gameplay::melee_duel::WindowEnd(slot, nowMs, swingMs, end);
        }

        bool g_installed = false;

        // The installed sinks live at namespace scope so the local-producer entry points below
        // can reuse them (they are stateless adapters, same objects Install() registers).
        HitRelBridge    s_hitrel;
        DodgeBridge     s_dodge;
        HitReactBridge  s_hitreact;
        ExchangeBridge  s_exchange;

    } // namespace

    void Install()
    {
        if constexpr (!kEnabled)
            return;
        if (g_installed)
            return;

        gameplay::melee_dodge::WindowOverrides windows;
        windows.exchange = &ExchangeWindowEnd;
        windows.duel = &DuelWindowEnd;
        net::match::hitrel::SetSink(&s_hitrel);
        gameplay::melee_dodge::SetSink(&s_dodge);
        gameplay::melee_dodge::SetWindowOverrides(windows);
        gameplay::melee_hit_react::SetSink(&s_hitreact);
        gameplay::melee_exchange::SetSink(&s_exchange);
        g_installed = true;
        melee_bridge::Log("melee sinks installed (wire/hit/dodge live; clip table/QTE/MV3/PSYNC gated)");
    }

    void OnLocalMeleeSwing()
    {
        if (!g_installed)
            return;
        // Announce our swing on the ext's 0x97 byte so peers open their dodge window. This is
        // in addition to the mod's NEVT_PLAYER_MELEE_ATTACK (which still drives the melee
        // animation); the two are different wires and neither renumbers a low event.
        s_dodge.SendSwing(kBroadcastSlot);
    }

    void OnLocalHit(uint8_t victimSlot, const uint8_t damage[0x16])
    {
        if (!damage)
            return;
        if (!g_installed) {
            // Boot ordering / dormant gate: keep the plain send the hook used before this bridge.
            net::session::SendEventRaw(static_cast<uint8_t>(NEVT_TAKE_DAMAGE),
                                       damage, 0x16, victimSlot);
            return;
        }
        if (victimSlot >= kMaxNetPlayers)
            return;
        uint8_t hit[0x16];
        std::memcpy(hit, damage, sizeof hit);
        net::match::hitrel::SendHit(hit, sizeof hit, victimSlot);
    }

    void Tick()
    {
        if (!g_installed)
            return;
        const uint64_t now = ClockMs();

        // The raw dodge bit is the local press producer (ext mdg_start). The engine still plays
        // its own dodge on the same bit; the core mirrors it, opens the peer-swing window and
        // sends 0x99. clipSid is 0: the contextual clip table is not ported (the ext's ps49
        // buffered-press path sends the same immune/dir pair with a zero clip).
        static bool s_dodgeHeld = false;
        const bool held = net::session::IsRunning() && net::match::liveness::Live()
                       && ::engine::pad::DodgeHeld();
        if (held && !s_dodgeHeld)
            gameplay::melee_dodge::Press(now, gameplay::melee_dodge::kFront, 0);
        s_dodgeHeld = held;

        net::match::hitrel::Tick();
        gameplay::melee_dodge::Tick(now);
        gameplay::melee_dodge::HeldTick(now, net::match::liveness::Live());
        gameplay::melee_hit_react::Tick();
        gameplay::melee_exchange::Tick(now, false);
    }

    void OnEvent(uint8_t sender, uint8_t type, const uint8_t* data, uint8_t size)
    {
        if (!g_installed || !data)
            return;
        const uint64_t now = ClockMs();
        switch (static_cast<NetEventType>(type)) {
        case NEVT_HIT_REL:
            net::match::hitrel::Receive(sender, data, size);
            break;
        case NEVT_DODGE_SWING:
            gameplay::melee_dodge::RecvSwing(sender, now, data, size);
            break;
        case NEVT_DODGE_PRESS:
            gameplay::melee_dodge::RecvPress(sender, now, data, size);
            break;
        case NEVT_DODGE_RES:
            gameplay::melee_dodge::RecvResult(sender, now, data, size);
            break;
        case NEVT_HITREACT2:
            gameplay::melee_hit_react::Receive(sender, data, size);
            break;
        case NEVT_MEX:
            gameplay::melee_exchange::Receive(sender, data, size);
            break;
        case NEVT_ARMOR_HIT:
            // A plain armour hit. Applying it lives behind HitRelSink::ApplyArmor (a no-op);
            // send-side capability / interception stays in the ext until that bridge lands.
            break;
        case NEVT_MV3:
            // The rollback netcore is engine-free via Mv3nIo; no bridge supplies it yet.
            break;
        case NEVT_PSYNC:
            // The paired wire core (MSG_PSYNC 0x4B lifecycle) is not ported yet.
            break;
        default:
            break;
        }
    }

    void Reset()
    {
        if (!g_installed)
            return;
        net::match::hitrel::Reset(true);
        gameplay::melee_dodge::Reset();
        gameplay::melee_duel::Reset();
        gameplay::melee_tell::Reset();
        gameplay::melee_hit_react::Reset();
        gameplay::melee_exchange::OnLeftMatch();
    }

    bool Installed() noexcept { return g_installed; }

} // namespace net::match::melee_bridge
