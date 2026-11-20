// gated integration seams for the pure downed core (see downed-bridge.hpp)

#include "downed-bridge.hpp"

#include "engine/frame.hpp"
#include "log.hpp"
#include "net/match/downed-state.hpp"
#include "net/match/match-liveness.hpp"
#include "net/online/net-session.hpp"
#include "net/protocol/net-protocol.hpp"

namespace net::match::downed_bridge {

    namespace {

        uint64_t ClockMs()
        {
            const double ms = engine::frame::NowMs();
            return ms > 0.0 ? static_cast<uint64_t>(ms) : 0ull;
        }

        void Log(const char* text)
        {
            if (text)
                TO1_LOG("[downed-bridge] %s", text);
        }

        // The clock, frame counter, local slot and match liveness are real; every wire / engine
        // / other-feature effect is a no-op. Send-side interception of MSG_DOWNED / MSG_REVIVE
        // stays in the ext's trampoline until the relay bridge is ported.
        class DownedSink final : public net::match::downed::Sink {
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
                return slot < net::match::downed::kMaxPeers &&
                       static_cast<int>(slot) != net::session::MySlot() &&
                       net::match::liveness::Live();
            }
            float LocalHp() override { return -1.0f; } // no observable pool source yet
            float BleedMul() override { return 1.0f; } // Fortitude / Heavy Hands unported
            float DownHpMul() override { return 1.0f; }
            void SendDowned(const uint8_t[6]) override {}
            void SendRevive(uint8_t) override {}
            void Log(const char* text) override { downed_bridge::Log(text); }

            void OnDownBegin(uint8_t, float) override {}
            void OnDownEnd(net::match::downed::End, uint8_t) override {}
            void OnBleedOut(uint8_t) override {}
            void OnProtectedHit(float) override {}
            void OnDownHit(float, float) override {}
            void OnRevived(uint8_t) override {}
            void OnPeerState(uint8_t, uint8_t, uint8_t) override {}
        };

        bool g_installed = false;

    } // namespace

    void Install()
    {
        if constexpr (!kEnabled)
            return;
        if (g_installed)
            return;

        static DownedSink s_sink;
        net::match::downed::SetSink(&s_sink);
        g_installed = true;
        downed_bridge::Log("inert downed sink installed (shadow mode; no ext cutover)");
    }

    void Tick()
    {
        if (!g_installed)
            return;
        net::match::downed::Tick();
    }

    void OnEvent(uint8_t sender, uint8_t type, const uint8_t* data, uint8_t size)
    {
        if (!g_installed || !data)
            return;
        switch (static_cast<NetEventType>(type)) {
        case NEVT_DOWNED:
            net::match::downed::ReceiveDowned(sender, data, size);
            break;
        case NEVT_REVIVE: {
            // The reviver is the sender; byte 0 (when present) names them explicitly.
            const uint8_t who = size >= 1 ? data[0] : sender;
            net::match::downed::ReceiveRevive(who);
            break;
        }
        default:
            break;
        }
    }

    void Reset()
    {
        if (!g_installed)
            return;
        net::match::downed::Reset();
    }

    bool Installed() noexcept
    {
        return g_installed;
    }

} // namespace net::match::downed_bridge
