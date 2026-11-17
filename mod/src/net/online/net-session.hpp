#pragma once

#include "net-common.hpp"
#include "net-protocol.hpp"

enum class NetRole : uint8_t {
    None = 0,
    Host,
    Client
};

namespace net::session {

    void HostGame();
    void JoinGame(const char* hostIP); 

    void HostGameRelay(const char* relayIp, uint16_t relayPort, uint32_t sessionId);
    void JoinGameRelay(const char* relayIp, uint16_t relayPort, uint32_t sessionId, uint8_t mySlot);

    void Leave();
    void LeaveInternal(bool sendGoodbye);
    void Update();

    bool     IsRunning();
    bool     IsConnected();
    bool     IsHost();
    NetRole  Role();
    int      MySlot();
    float    Now();
    void     PrintStatus();

    const char* LocalName();
    void        SetLocalName(const char* name);
    const char* LocalLook();
    void        SetLocalLook(const char* look);

    void        EnsureLocalTeamLook();

    void SendEventRaw(uint8_t type, const void* data, uint8_t size, uint8_t targetSlot = kBroadcastSlot);

    template <typename T>
    inline void SendEvent(NetEventType type, const T& payload, uint8_t targetSlot = kBroadcastSlot)
    {
        static_assert(sizeof(T) <= 200, "event payload too large");
        SendEventRaw(static_cast<uint8_t>(type), &payload, static_cast<uint8_t>(sizeof(T)), targetSlot);
    }

    inline void SendEvent(NetEventType type, uint8_t targetSlot = kBroadcastSlot)
    {
        SendEventRaw(static_cast<uint8_t>(type), nullptr, 0, targetSlot);
    }

    inline void SendEventSid(NetEventType type, uint64_t sid, uint8_t targetSlot = kBroadcastSlot)
    {
        SendEvent<uint64_t>(type, sid, targetSlot);
    }

    void SendEventString(NetEventType type, const char* str, uint8_t targetSlot = kBroadcastSlot);

}