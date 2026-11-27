// Native integration seam for the pure downed core (see downed-bridge.hpp).
// The legacy ext is the only cutover: every send, health write, death note and
// revive producer here no-ops while it is loaded.

#include "downed-bridge.hpp"

#include "boot.hpp"
#include "engine/frame.hpp"
#include "engine/local-player.hpp"
#include "engine/pad.hpp"
#include "engine/raycast.hpp"
#include "engine/script.hpp"
#include "engine/window.hpp"
#include "log.hpp"
#include "net/game/net-replication.hpp"
#include "net/match/downed-state.hpp"
#include "net/match/match-liveness.hpp"
#include "net/match/net-player.hpp"
#include "net/online/net-session.hpp"
#include "net/protocol/net-protocol.hpp"
#include "net/ui/death-ui.hpp"
#include "net/ui/kill-feed-icons.hpp"
#include "sid/sid.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace net::match::downed_bridge {

    namespace {

        constexpr float   kReviveSecs   = 5.0f;  // no Reviver level feed wired into this bridge yet
        constexpr float   kReviveReach  = 2.5f;
        constexpr float   kReviveKeep   = 3.5f;
        constexpr uint64_t kDeathWindowFrames = 90;

        uint64_t ClockMs()
        {
            const double ms = engine::frame::NowMs();
            return ms > 0.0 ? static_cast<uint64_t>(ms) : 0ull;
        }

        bool NativeOwner()
        {
            return kEnabled && !boot::ExtLoaded() && net::match::liveness::Live();
        }

        void Log(const char* text)
        {
            if (text)
                TO1_LOG("[downed-bridge] %s", text);
        }

        bool ValidSlot(uint8_t slot)
        {
            return slot < downed::kMaxPeers &&
                   static_cast<int>(slot) == net::session::MySlot();
        }

        // The damage receiver stamps this before BeginDown(): the kill-feed down row needs
        // the weapon the ext's KfDownLocal received with its DOWNED broadcast.
        uint64_t gLastWeapon = 0;

        void SendRaw(uint8_t type, const void* body, uint8_t size)
        {
            if (!NativeOwner())
                return;
            net::session::SendEventRaw(type, body, size);
        }

        float DownPool()
        {
            return downed::kDownHp; // native Fortitude II pool bonus is not sourced
        }

        void SetPool(float hp)
        {
            if (!NativeOwner())
                return;
            PLAYER::SET_HEALTH(static_cast<int>(hp > 0.0f ? hp : 0.0f));
        }

        // Sends / engine effects for the pure core. Every producer is gated through
        // NativeOwner(), so a loaded ext keeps sole ownership of the same wire bytes.
        class DownedSink final : public downed::Sink {
        public:
            uint64_t NowMs() override { return ClockMs(); }
            uint64_t Frame() override { return engine::frame::Count(); }
            uint8_t LocalSlot() override
            {
                const int slot = net::session::MySlot();
                return slot < 0 ? 255u : static_cast<uint8_t>(slot);
            }
            bool InMatch() override { return net::match::liveness::Live(); }
            bool PeerActive(uint8_t slot) override
            {
                return slot < downed::kMaxPeers &&
                       static_cast<int>(slot) != net::session::MySlot() &&
                       NetPlayers::BySlot(slot).IsActiveRemote();
            }
            float LocalHp() override { return engine::local_player::Health(); }
            float BleedMul() override { return 1.0f; }
            float DownHpMul() override { return 1.0f; }
            void SendDowned(const uint8_t body[6]) override
            {
                SendRaw(static_cast<uint8_t>(NEVT_DOWNED), body, 6);
            }
            void SendRevive(uint8_t target) override
            {
                SendRaw(static_cast<uint8_t>(NEVT_REVIVE), &target, 1);
            }
            void Log(const char* text) override { downed_bridge::Log(text); }

            void OnDownBegin(uint8_t downer, float bleedSecs) override
            {
                if (!NativeOwner())
                    return;
                SetPool(DownPool());
                net::ui::killfeed::NoteLocalDown(
                    downer, gLastWeapon ? gLastWeapon : SID("abby-unarmed"));
                char text[160];
                std::snprintf(text, sizeof text,
                              "downed by slot%u - %.1f s bleed-out, %s",
                              downer, bleedSecs, "50 hp pool");
                Log(text);
            }
            void OnDownEnd(downed::End why, uint8_t source) override
            {
                const int slot = net::session::MySlot();
                if (!NativeOwner() || slot < 0)
                    return;
                net::ui::killfeed::NoteDownEnd(static_cast<uint8_t>(slot));
                if (why == downed::End::kRevived) {
                    SetPool(downed::kReviveHp);
                    return;
                }
                // A downed lethal / bleed-out only feeds the existing Death-edge path.
                // The confirmed kill still owns the death card, kill row and award.
                downed::SetLocalDying(true);
                downed::SetDyingUntilFrame(
                    engine::frame::Count() + kDeathWindowFrames);
                net::ui::death_ui::NoteEdge(
                    net::ui::death_ui::core::EdgeKind::DownedLethal,
                    ClockMs(), source, gLastWeapon);
            }
            void OnBleedOut(uint8_t downer) override
            {
                if (!NativeOwner())
                    return;
                net::ui::death_ui::NoteEdge(
                    net::ui::death_ui::core::EdgeKind::DownedLethal,
                    ClockMs(), downer, gLastWeapon);
            }
            void OnProtectedHit(float dmg) override
            {
                if (!NativeOwner()) return;
                char text[128];
                std::snprintf(text, sizeof text,
                              "downed: first-second protection ignored %.0f damage", dmg);
                Log(text);
            }
            void OnDownHit(float dmg, float hpLeft) override
            {
                if (!NativeOwner()) return;
                char text[128];
                std::snprintf(text, sizeof text,
                              "downed: pool hit %.0f, %.0f left", dmg, hpLeft);
                Log(text);
            }
            void OnRevived(uint8_t reviver) override
            {
                if (!NativeOwner()) return;
                SetPool(downed::kReviveHp);
                char text[128];
                std::snprintf(text, sizeof text,
                              "revived by slot%u - 40 hp", reviver);
                Log(text);
            }
            void OnPeerState(uint8_t, uint8_t, uint8_t) override {}
        };

        bool g_installed = false;

        // ---- native revive-reach producer -------------------------------------------------
        struct ReachState {
            int      target = -1;
            uint64_t startMs = 0;
            bool     on = false;
            bool     complete = false;
            std::array<uint64_t, kMaxNetPlayers> losJob{};
            std::array<bool,      kMaxNetPlayers> losBlocked{};
        } gReach;

        void PollLos(int slot, bool want)
        {
            if (slot < 0 || slot >= kMaxNetPlayers)
                return;
            if (!want) {
                gReach.losJob[slot] = 0;
                gReach.losBlocked[slot] = false;
                return;
            }
            if (gReach.losJob[slot]) {
                const float t = engine::raycast::RayT(gReach.losJob[slot]);
                if (t != -1.0f) {
                    gReach.losBlocked[slot] = t >= 0.0f && t < 0.98f;
                    gReach.losJob[slot] = 0;
                }
            }
            if (gReach.losJob[slot])
                return;
            const NetPlayer& p = NetPlayers::BySlot(slot);
            if (!p.npcSid)
                return;
            const Transform me = LOCATOR::GET_TRANSFORM(SID("player"));
            const Transform them = LOCATOR::GET_TRANSFORM(p.npcSid);
            gReach.losJob[slot] = engine::raycast::KickWorldRay(
                Vector3{ me.Position.x, me.Position.y + 1.0f, me.Position.z },
                Vector3{ them.Position.x, them.Position.y + 0.7f, them.Position.z },
                engine::raycast::kMaskStaticWorld, SID("player"));
        }

        bool LosClear(int slot) noexcept
        {
            return slot >= 0 && slot < kMaxNetPlayers && !gReach.losBlocked[slot];
        }

        int FindReviveTarget()
        {
            const Transform me = LOCATOR::GET_TRANSFORM(SID("player"));
            int best = -1;
            float bestDist = kReviveReach * kReviveReach;
            for (const NetPlayer& p : NetPlayers::Remotes()) {
                if (!NetPlayers::SlotIsFriendly(p.netSlot) || !p.npcSid || p.dead)
                    continue;
                if (downed::PeerState(static_cast<uint8_t>(p.netSlot)) !=
                    downed::kStateDown)
                    continue;
                const Transform them = LOCATOR::GET_TRANSFORM(p.npcSid);
                const float dx = them.Position.x - me.Position.x;
                const float dy = them.Position.y - me.Position.y;
                const float dz = them.Position.z - me.Position.z;
                const float r = gReach.on ? kReviveKeep : kReviveReach;
                if (!(dx * dx + dy * dy + dz * dz <= r * r))
                    continue;
                const float d = dx * dx + dz * dz;
                if (d >= bestDist)
                    continue;
                bestDist = d;
                best = p.netSlot;
            }
            return best;
        }

        void SendReach(uint8_t target, bool on)
        {
            const uint8_t body[4] = {
                target, static_cast<uint8_t>(on ? 1 : 0),
                static_cast<uint8_t>(kReviveSecs * 10.0f), 0
            };
            SendRaw(static_cast<uint8_t>(NEVT_REACH), body, sizeof body);
        }

        void StopReach(uint8_t target)
        {
            const int local = net::session::MySlot();
            if (local >= 0 && target < downed::kMaxPeers)
                downed::SetHold(target, static_cast<uint8_t>(local), false, 0.0f, 0);
            if (gReach.on)
                SendReach(target, false);
            gReach.target = -1;
            gReach.on = false;
            gReach.complete = false;
            gReach.startMs = 0;
            PollLos(gReach.target, false);
        }

        void TickReviveProducer()
        {
            const int local = net::session::MySlot();
            if (local < 0 || local >= downed::kMaxPeers || downed::IsDown() ||
                engine::local_player::Health() <= 0.0f)
                return;

            const bool held = engine::pad::Held("interact") ||
                              engine::window::KeyDown('E');
            const int target = held ? FindReviveTarget() : -1;
            if (target != gReach.target) {
                if (gReach.target >= 0)
                    StopReach(static_cast<uint8_t>(gReach.target));
                gReach.target = target;
            }
            if (target < 0)
                return;

            PollLos(target, true);
            if (!LosClear(target))
                return;

            const uint64_t now = ClockMs();
            if (!gReach.on) {
                gReach.on = true;
                gReach.complete = false;
                gReach.startMs = now;
                downed::SetHold(static_cast<uint8_t>(target),
                                static_cast<uint8_t>(local), true,
                                kReviveSecs, now);
                SendReach(static_cast<uint8_t>(target), true);
            }
            if (!gReach.complete &&
                downed::ReviveProgress(static_cast<uint8_t>(target)) >= 1.0f) {
                gReach.complete = true;
                downed::CompleteRevive(static_cast<uint8_t>(target),
                                       static_cast<uint8_t>(local));
                StopReach(static_cast<uint8_t>(target));
            }
        }

    } // namespace

    void Install()
    {
        if constexpr (!kEnabled)
            return;
        if (g_installed || boot::ExtLoaded())
            return;
        static DownedSink s_sink;
        downed::SetSink(&s_sink);
        g_installed = true;
        Log("native downed sink installed");
    }

    void Tick()
    {
        if (!g_installed || boot::ExtLoaded())
            return;
        downed::Tick();
        TickReviveProducer();
    }

    void OnEvent(uint8_t sender, uint8_t type, const uint8_t* data, uint8_t size)
    {
        if (!g_installed || boot::ExtLoaded() || !data ||
            !net::match::liveness::Live())
            return;
        const auto event = static_cast<NetEventType>(type);
        if (event == NEVT_DOWNED) {
            downed::ReceiveDowned(sender, data, size);
            return;
        }
        if (event == NEVT_REVIVE) {
            if (size < 1 || sender >= downed::kMaxPeers)
                return;
            // [u8 revived target], sent by the reviver. Credit / trust comes from `sender`.
            downed::ReceiveRevive(data[0], sender);
            return;
        }
        if (event != NEVT_REACH || size < 2 || sender >= downed::kMaxPeers ||
            !NetPlayers::SlotIsFriendly(sender))
            return;
        const uint8_t target = data[0];
        const bool on = data[1] != 0;
        const float secs = size >= 3 && data[2] ? data[2] / 10.0f : kReviveSecs;
        downed::SetHold(target, sender, on, secs, ClockMs());
        const int local = net::session::MySlot();
        if (target == local && local >= 0) {
            if (on)
                downed::SetLocalReviver(sender, secs);
            else
                downed::ClearLocalReviver();
        }
    }

    void Reset()
    {
        if (gReach.target >= 0 && gReach.on)
            SendReach(static_cast<uint8_t>(gReach.target), false);
        gReach = ReachState{};
        gLastWeapon = 0;
        if (!g_installed)
            return;
        downed::Reset();
    }

    bool Installed() noexcept
    {
        return g_installed && !boot::ExtLoaded();
    }

    DamageDecision NoteLocalDamage(uint8_t attacker, uint64_t weapon, float damage,
                                   bool lethalClaim)
    {
        DamageDecision out;
        out.outcome = DamageOutcome::kNormal;
        out.applyDamage = damage;
        if (!Installed() || !(damage > 0.0f))
            return out;
        gLastWeapon = weapon;

        const float hp = engine::local_player::Health();
        const bool executionMarker = damage >= 100000.0f;
        if (downed::IsDown()) {
            if (executionMarker || (hp > 0.0f && damage >= hp)) {
                downed::SetLocalDying(true);
                downed::SetDyingUntilFrame(
                    engine::frame::Count() + kDeathWindowFrames);
                net::ui::death_ui::NoteEdge(
                    net::ui::death_ui::core::EdgeKind::DownedLethal,
                    ClockMs(), attacker, weapon);
                out.outcome = DamageOutcome::kDying;
                out.applyDamage = 0.0f;
                return out;
            }
            // The first second ignores pool hits; later hits are absorbed by the pool and
            // only shorten the bleed-out clock (the core owns that rule).
            const uint64_t now = ClockMs();
            if (downed::DownMs() && now - downed::DownMs() < downed::kProtectMs) {
                out.outcome = DamageOutcome::kIgnored;
                out.applyDamage = 0.0f;
                return out;
            }
            if (downed::HitWhileDown(damage, hp > 0.0f ? hp : DownPool())) {
                downed::SetLocalDying(true);
                downed::SetDyingUntilFrame(
                    engine::frame::Count() + kDeathWindowFrames);
                net::ui::death_ui::NoteEdge(
                    net::ui::death_ui::core::EdgeKind::DownedLethal,
                    ClockMs(), attacker, weapon);
                out.outcome = DamageOutcome::kDying;
                out.applyDamage = 0.0f;
            }
            else {
                out.outcome = DamageOutcome::kIgnored;
                out.applyDamage = 0.0f;
            }
            return out;
        }

        if (executionMarker || !lethalClaim || hp <= 0.0f || damage < hp)
            return out;
        if (!downed::BeginDown(attacker, 0, false))
            return out;
        // The engine hit is clamped to leave the down pool intact; the sink writes the
        // exact pool after FireAttack through CommitDownPool().
        out.outcome = DamageOutcome::kDowned;
        out.applyDamage = hp > DownPool() ? hp - DownPool() : 0.0f;
        return out;
    }

    void CommitDownPool()
    {
        if (Installed() && downed::IsDown())
            SetPool(DownPool());
    }

} // namespace net::match::downed_bridge
