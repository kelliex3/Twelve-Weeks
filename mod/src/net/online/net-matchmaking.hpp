#pragma once

#include <cstdint>
#include "net-matchmaker-protocol.hpp"
#include "match.hpp"

namespace net::matchmaking {

    enum class State {
        Idle,        // not connected
        Connecting,  // TCP connect to the matchmaker is in flight
        Connected,   // connected + present at the MP menu (said HELLO), not searching yet
        Searching,   // queued, waiting for a match
        Matched,     // MATCH_FOUND received -- GetMatch() is valid
        Failed,      // connect or socket error (see the last [MM] log line)
    };

    using MatchInfo = net::match::MatchInfo;

    void Connect(const char* playerName);

    void FindMatch(const char* playerName, uint8_t mode, uint8_t map,
        uint64_t primaryWeapon, uint64_t secondaryWeapon);

    void Cancel();
    void Tick();
    State GetState();
    inline const MatchInfo* GetMatch()
    {
        net::match::Match& m = net::match::Get();
        return m.IsMatchmade() ? &m.info : nullptr;
    }

    int         LobbyNameCount();
    const char* LobbyName(int i);

    bool SlotLoadout(int slot, StringId64& primaryWeapon, StringId64& secondaryWeapon);

    int MyTeam();
    int SlotTeam(int slot);

    bool VersionRejected();
    bool TokenRejected();

    void SendReady();
    bool StartReceived();
    bool Aborted();

    bool TakeBroadcast(char* out, int cap);

    void ReportResult(int winner);
    bool Authed();
    bool ReportTamper(const MMTamper& report);
    void RequestLeaderboard();
    const MMLeaderEntry* LeaderboardEntry(int i);
    void     RequestOnlineCount();
    uint32_t OnlineCount();
    int LobbyPhase();
    int LobbyCountdown();

    uint32_t    ServerAddr();
    const char* ServerHost();
    uint16_t    ServerPort();

}