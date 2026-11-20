// ported integration seams for the pure melee cores (see melee-bridge.hpp)

#include "melee-bridge.hpp"

#include "engine/frame.hpp"
#include "log.hpp"
#include "net/online/net-session.hpp"
#include "net/protocol/net-protocol.hpp"
#include "net/match/hit-rel.hpp"
#include "net/match/match-liveness.hpp"
#include "gameplay/melee-dodge.hpp"
#include "gameplay/melee-hit-react.hpp"
#include "gameplay/melee-exchange.hpp"

#include <cstdint>

namespace net::match::melee_bridge {

    namespace {

        uint64_t ClockMs()
        {
            const double ms = engine::frame::NowMs();
            return ms > 0.0 ? static_cast<uint64_t>(ms) : 0ull;
        }

        void Log(const char* text)
        {
            if (text)
                TO1_LOG("[melee-bridge] %s", text);
        }

        // ---- hitrel -----------------------------------------------------------------------
        // The clock, match state and local slot are real; every wire / apply effect is a no-op.
        // Send-side interception (HrSendHit / SendArmor / NoteOriginal) is a separate gate: the
        // mod calls SendEvent directly, so there is no send trampoline to hook here yet.
        class HitRelBridge final : public net::match::hitrel::Sink {
        public:
            uint64_t NowMs() override { return ClockMs(); }
            bool InMatch() override { return net::match::liveness::Live(); }
            bool PeerActive(uint8_t slot) override
            {
                return slot < 8 && static_cast<int>(slot) != net::session::MySlot()
                    && net::match::liveness::Live();
            }
            uint8_t LocalSlot() override
            {
                const int s = net::session::MySlot();
                return s < 0 ? 255u : static_cast<uint8_t>(s);
            }
            void SendWire(uint8_t, const uint8_t*, uint8_t) override {}
            void SendPlainHit(uint8_t, const uint8_t[0x16]) override {}
            void SendPlainArmor(uint8_t, const uint8_t[11]) override {}
            void SendPlainBurst(uint8_t, const uint8_t*, uint8_t) override {}
            void ApplyHit(uint8_t, const uint8_t[0x16]) override {}
            void ApplyArmor(uint8_t, const uint8_t[11]) override {}
            void ApplyBurst(uint8_t, const uint8_t*, uint8_t) override {}
            void Log(const char* text) override { melee_bridge::Log(text); }
        };

        // ---- dodge ------------------------------------------------------------------------
        class DodgeBridge final : public gameplay::melee_dodge::Sink {
        public:
            void SendSwing(uint8_t) override {}
            void SendPress(uint8_t, uint8_t, uint8_t, uint64_t) override {}
            void SendResult(uint8_t) override {}
            void Log(const char* text) override { melee_bridge::Log(text); }
        };

        // ---- hit reactions -----------------------------------------------------------------
        class HitReactBridge final : public gameplay::melee_hit_react::Sink {
        public:
            uint64_t NowMs() override { return ClockMs(); }
            // The SP clip table (melee_hitreact_rm.h) is private / unported: no clip means no
            // reaction, so the victim / attacker state machines stay inactive in shadow mode.
            bool PickClip(const gameplay::melee_hit_react::Blow&, int, bool, uint64_t,
                          uint64_t*, uint16_t*, int*) override { return false; }
            uint64_t ExitMsOf(uint64_t, uint16_t cutMs) override { return cutMs; }
            void PlayLocalReaction(uint64_t, uint16_t, int) override {}
            void StopLocalReaction(uint64_t, float) override {}
            void PlayPuppetReaction(uint8_t, uint64_t, uint16_t, uint64_t) override {}
            void StopPuppetReaction(uint8_t, uint64_t, float) override {}
            void PainSound(uint8_t, int, bool) override {}
            void Punch(uint8_t, int) override {}
            void HitFxConfirm(uint8_t) override {}
            bool PredBlocked(uint8_t) override { return true; }  // no observable puppet / dodge state
            float RttMs(uint8_t) override { return 0.0f; }        // no relay RTT source yet
            void SendMessage(const uint8_t[gameplay::melee_hit_react::kMessageSize]) override {}
            void Log(const char* text) override { melee_bridge::Log(text); }
        };

        // ---- exchange ----------------------------------------------------------------------
        class ExchangeBridge final : public gameplay::melee_exchange::Sink {
        public:
            uint64_t NowMs() override { return ClockMs(); }
            uint8_t LocalSlot() override
            {
                const int s = net::session::MySlot();
                return s < 0 ? 255u : static_cast<uint8_t>(s);
            }
            float OnewayMs(uint8_t) override { return 0.0f; }     // no psync one-way source yet
            uint16_t TellTc(int, uint16_t authored) override { return authored; }
            uint16_t QteFlags() override { return 0; }
            uint16_t QtePressOffset() override { return 0; }
            uint8_t QteCap() override { return 0; }
            bool PredBlocked(uint8_t) override { return true; }  // no busy / down / reach gate yet
            void Send(uint8_t, const uint8_t*, uint8_t) override {}
            void Log(const char* text) override { melee_bridge::Log(text); }
            void PlayFreeSwing() override {}
            void FireSwing() override {}
            void ReplayHit(uint8_t) override {}
            void PlayMiss() override {}
            void BeginQte(uint8_t) override {}
            void EndQte() override {}
        };

        bool g_installed = false;

    } // namespace

    void Install()
    {
        if constexpr (!kEnabled)
            return;
        if (g_installed)
            return;

        static HitRelBridge    s_hitrel;
        static DodgeBridge     s_dodge;
        static HitReactBridge  s_hitreact;
        static ExchangeBridge  s_exchange;

        net::match::hitrel::SetSink(&s_hitrel);
        gameplay::melee_dodge::SetSink(&s_dodge);
        gameplay::melee_hit_react::SetSink(&s_hitreact);
        gameplay::melee_exchange::SetSink(&s_exchange);
        g_installed = true;
        melee_bridge::Log("inert melee sinks installed (shadow mode; no ext cutover)");
    }

    void Tick()
    {
        if (!g_installed)
            return;
        const uint64_t now = ClockMs();
        net::match::hitrel::Tick();
        gameplay::melee_dodge::Tick(now);
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
        gameplay::melee_hit_react::Reset();
        gameplay::melee_exchange::OnLeftMatch();
    }

    bool Installed() noexcept { return g_installed; }

} // namespace net::match::melee_bridge
