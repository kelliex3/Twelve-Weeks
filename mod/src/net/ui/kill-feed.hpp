#pragma once

#include <string>
#include <cstdint>

struct KillFeedEntry {
    std::string text;
    uint32_t    color;
    float       timeRemaining;
};

constexpr unsigned char kTeamBlueR = 80,  kTeamBlueG = 160, kTeamBlueB = 255;
constexpr unsigned char kTeamRedR  = 255, kTeamRedG  = 80,  kTeamRedB  = 80;

void AddKillFeedEntry(const std::string& killerName, const std::string& victimName, bool killerFriendly, bool victimFriendly, uint32_t color = 0xFFFFFFFF);
void AddDeathFeedEntry(const std::string& victimName, bool victimFriendly, uint32_t color = 0xFFFFFFFF);

void UpdateKillFeed();
void DrawKillFeed();
void ResetKillFeed();

void DrawRadar();
extern bool g_radarShowAll;