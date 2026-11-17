#include "kill-feed.hpp"
#include "net-common.hpp"
#include "net-player.hpp"
#include "match.hpp"
#include "net-engine.hpp"
#include "net-matchmaking.hpp"
#include "net-replication.hpp"
#include "net-debug.hpp"
#include "gameplay/perks/definitions/hawk_eyed.hpp"

#include <deque>
#include <cstdio>
#include <cmath>
#include <net-gui2.hpp>

static constexpr float ENTRY_LIFETIME = 5.0f;
static constexpr int   MAX_ENTRIES = 5;
static constexpr float FONT_SIZE = 1.0f;
static constexpr float RIGHT_MARGIN = 30.0f;
static constexpr float VIRTUAL_SCREEN_W = 1280.0f; // debug-draw is 720p space
static constexpr float KILLFEED_TOP_MARGIN = 50.0f;

static constexpr float RADAR_LEFT = 30.0f;
static constexpr float RADAR_TOP = 30.0f;
static constexpr float RADAR_SIZE = 150.0f;
static constexpr float RADAR_RANGE = 30.0f;            // meters to radar edge
static constexpr float RADAR_DOT = 2.5f;             // rim inset for clamped blips
static constexpr float kRadarFadeIn = 0.2f;             // seconds to full
static constexpr float kRadarFadeOut = 1.5f;             // seconds to gone

static constexpr float RADAR_RIGHT = RADAR_LEFT + RADAR_SIZE;
static constexpr float RADAR_BOTTOM = RADAR_TOP + RADAR_SIZE;
static constexpr float RADAR_CX = RADAR_LEFT + RADAR_SIZE * 0.5f;
static constexpr float RADAR_CY = RADAR_TOP + RADAR_SIZE * 0.5f;
static constexpr float UNITS_PER_PX = RADAR_RANGE / (RADAR_SIZE * 0.5f);

// Radar glyphs (debug-draw font): YOU is the larger blip, enemies the smaller.
static const char* kYouGlyph = "\u0316";
static const char* kEnemyGlyph = "\u0317";
static const char* kBgGlyph = "\u0317";
static const char* kSupplyBoxGlyph = "\u0318";
static constexpr float YOU_BLIP_FONT = 0.85f;
static constexpr float ENEMY_BLIP_FONT = 0.55f;
static constexpr float SUPPLYBOX_BLIP_FONT = 0.65f;
static constexpr float SUPPLYBOX_PICKUP_BLIP_FONT = 0.3f;

bool     g_radarShowAll = false;

// kill feed
static std::deque<KillFeedEntry> s_entries;

void AddKillFeedEntry(const std::string& killer,
    const std::string& victim,
    bool killerFriendly, bool victimFriendly, uint32_t color)
{
    const char* killerColor = killerFriendly ? "blue" : "red";
    const char* victimColor = victimFriendly ? "blue" : "red";

    char buffer[256];
    snprintf(buffer, sizeof(buffer),
        "<color=%s>%s</color> killed <color=%s>%s</color>",
        killerColor, killer.c_str(), victimColor, victim.c_str());

    KillFeedEntry e;
    e.text = buffer;
    e.color = color;
    e.timeRemaining = ENTRY_LIFETIME;

    s_entries.push_back(e);                  // newest at the bottom, oldest at the top
    while (s_entries.size() > MAX_ENTRIES)
        s_entries.pop_front();

    TO1_LOG("[FEED] %s\n", e.text.c_str());
}

void AddDeathFeedEntry(const std::string& victim, bool victimFriendly, uint32_t color)
{
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "<color=%s>%s</color> died",
        victimFriendly ? "blue" : "red", victim.c_str());

    KillFeedEntry e;
    e.text = buffer;
    e.color = color;
    e.timeRemaining = ENTRY_LIFETIME;

    s_entries.push_back(e);
    while (s_entries.size() > MAX_ENTRIES)
        s_entries.pop_front();

    TO1_LOG("[FEED] %s\n", e.text.c_str());
}

void UpdateKillFeed()
{
    float dt = TIME::GET_DELTA_TIME();
    for (auto& e : s_entries)
        e.timeRemaining -= dt;
    while (!s_entries.empty() && s_entries.front().timeRemaining <= 0.0f)
        s_entries.pop_front();
}

void DrawKillFeed()
{
    float y = KILLFEED_TOP_MARGIN;
    for (const auto& e : s_entries) {
        auto  sz = DebugDrawCommon::GetTextWidthHeightWithTags(e.text.c_str(), FONT_SIZE);
        float x = VIRTUAL_SCREEN_W - sz.x - RIGHT_MARGIN;

        // White base; the <color=...> tags tint the names. Fade over the last second.
        uint8_t alpha = 255;
        if (e.timeRemaining < 1.0f)
            alpha = static_cast<uint8_t>(e.timeRemaining * 255.0f);

        DebugDrawCommon::DrawTextPrintV(x, y, FONT_SIZE,
            _rgba(255, 255, 255, alpha), "%s", e.text.c_str());
        y += sz.y + 7.0f;
    }
}

// radar
static Vector3 QuatGetForward(const Quaternion& q)
{
    return Vector3{
        2.0f * (q.x * q.z + q.w * q.y),
        2.0f * (q.y * q.z - q.w * q.x),
        1.0f - 2.0f * (q.x * q.x + q.y * q.y),
    };
}

std::pair<float, float> ClampPositionRadarIconToBorder(float dx, float dz, float relX, float relZ, float radarX, float radarY) {

    float distSq = dx * dx + dz * dz;
    if (distSq > RADAR_RANGE * RADAR_RANGE) {
        if (net::gui2::GetRadarMode() == net::gui2::RadarMode::Square)
        {
            // Find how far we can travel along the direction before
            // hitting one of the square's edges.
            float halfW = (RADAR_RIGHT - RADAR_LEFT) * 0.5f - RADAR_DOT;
            float halfH = (RADAR_BOTTOM - RADAR_TOP) * 0.5f - RADAR_DOT;

            float ax = fabsf(relX);
            float az = fabsf(relZ);

            float scaleX = halfW / ax;
            float scaleZ = halfH / az;

            float scale = fminf(scaleX, scaleZ);

            radarX = RADAR_CX - relX * scale;
            radarY = RADAR_CY - relZ * scale;
        }
        else
        {
            float dist = sqrtf(distSq);
            radarX = RADAR_CX - (relX / dist) * (RADAR_SIZE * 0.5f - RADAR_DOT);
            radarY = RADAR_CY - (relZ / dist) * (RADAR_SIZE * 0.5f - RADAR_DOT);
        }

    }
    return { radarX, radarY };
}

void DrawRadar()
{
    float dt = TIME::GET_DELTA_TIME();
    if (dt > 0.1f) dt = 0.1f;                          // clamp hitches

    if (!ENTITY::DOES_EXIST(SID("player")))
        return;

    Transform me = LOCATOR::GET_TRANSFORM(SID("player"));

    // Camera yaw so "up" on the radar is where you're looking.
    uint64_t   camLoc = DC::Call<"camera-locator">(0);
    Quaternion camRot = LOCATOR::GET_LOCATOR_ROTATION(camLoc);
    Vector3    fwd = QuatGetForward(camRot);
    float yaw = atan2f(fwd.x, fwd.z);
    float c = cosf(yaw), s = sinf(yaw);

    // translucent background
    uint32_t radarBGColor = _rgba(0, 0, 0, 160);

    if (net::gui2::GetRadarMode() == net::gui2::RadarMode::Square)
    {
        DebugDrawCommon::DrawRectangle(RADAR_LEFT, RADAR_TOP, RADAR_RIGHT, RADAR_BOTTOM,
            radarBGColor);
    }
    else
    {
        auto radarCircleBgSz = DebugDrawCommon::GetTextWidthHeight(kBgGlyph, 8.5f);
        DebugDrawCommon::DrawTextPrintV(RADAR_CX - radarCircleBgSz.x * 0.5f, RADAR_CY - radarCircleBgSz.y * 0.5f, 8.5f, radarBGColor, "%s", kBgGlyph);
    }


    // Show supply boxes dots in the radar if not opened
    for (NetPlayers::SupplyBox& box : NetPlayers::supplyBoxes) {

        // dropped ammo from killed enemies doesn't show up on radar. one for polish in the future
        for (StringId64& item : box.spawnedItems) {

            if (box.spawnedItems.empty()) continue;

            uintptr_t ptr = 0;

            Transform itemTrans = LOCATOR::GET_TRANSFORM(item);
            
            if (itemTrans.Position.x == 0.0f && itemTrans.Position.y == 0.0f && itemTrans.Position.x == 0.0f) continue; //Hacky fix for: item getting teleported to world origin on pick up which looks bad so we stop it rendering

            float itemDx = itemTrans.Position.x - me.Position.x;
            float itemDz = itemTrans.Position.z - me.Position.z;

            // rotate into view space ("up" = forward)
            float itemRelX = itemDx * c - itemDz * s;
            float itemRelZ = itemDx * s + itemDz * c;

            // map to the radar; subtract so world-right maps to radar-right (un-mirrored)
            float itemRadarX = RADAR_CX - itemRelX / UNITS_PER_PX;
            float itemRadarY = RADAR_CY - itemRelZ / UNITS_PER_PX;

            // out of range -> clamp to the rim, kept RADAR_DOT inside the edge
            auto pos = ClampPositionRadarIconToBorder(itemDx, itemDz, itemRelX, itemRelZ, itemRadarX, itemRadarY);

            itemRadarX = pos.first;
            itemRadarY = pos.second;

            auto itemSize = DebugDrawCommon::GetTextWidthHeight(kEnemyGlyph, SUPPLYBOX_PICKUP_BLIP_FONT);
            DebugDrawCommon::DrawTextPrintV(itemRadarX - itemSize.x * 0.5f, itemRadarY - itemSize.y * 0.5f,
                SUPPLYBOX_PICKUP_BLIP_FONT, _rgba(66.0f, 150.0f, 169.0f, 100.0f),
                "%s", kEnemyGlyph);
        }

        if (!box.CanOpenSupplyBox(net::match::Get().NowSec())) continue;

        uintptr_t ptr = 0;

        Transform boxTrans = LOCATOR::GET_TRANSFORM(box.boxSID);
        float dx = boxTrans.Position.x - me.Position.x;
        float dz = boxTrans.Position.z - me.Position.z;

        // rotate into view space ("up" = forward)
        float relX = dx * c - dz * s;
        float relZ = dx * s + dz * c;

        // map to the radar; subtract so world-right maps to radar-right (un-mirrored)
        float radarX = RADAR_CX - relX / UNITS_PER_PX;
        float radarY = RADAR_CY - relZ / UNITS_PER_PX;

        // out of range -> clamp to the rim, kept RADAR_DOT inside the edge
        auto pos = ClampPositionRadarIconToBorder(dx, dz, relX, relZ, radarX, radarY);

        radarX = pos.first;
        radarY = pos.second;

        auto    boxSize = DebugDrawCommon::GetTextWidthHeight(kSupplyBoxGlyph, SUPPLYBOX_BLIP_FONT);
        DebugDrawCommon::DrawTextPrintV(radarX - boxSize.x * 0.5f, radarY - boxSize.y * 0.5f,
            SUPPLYBOX_BLIP_FONT, _rgba(220, 220, 220, 255),
            "%s", kSupplyBoxGlyph);      

    }

    // remotes -- TEAMMATES are always shown (in blue); opponents only leak onto the radar while
    // sprinting-and-moving (or while AIM-MARKED, or via the g_radarShowAll toggle), which is the
    // whole point of the sprint tell.
    const int myTeam = net::matchmaking::MyTeam();
    for (int slot = 0; slot < kMaxNetPlayers; slot++) {
        NetPlayer& p = NetPlayers::BySlot(slot);

        uintptr_t ptr = 0;
        bool visible = false;
        bool teammate = false;
        if (p.IsActiveRemote() && p.spawned) {
            ptr = p.Ptr();
            const int t = net::match::Get().TeamOf(slot);   // roster team, else matchmaker copy (S7)
            teammate = (myTeam >= 0 && t >= 0 && t == myTeam);
            // IsSlotMarked is only ever true for enemies, so it cannot reveal a teammate here.
            visible = ptr && (teammate || g_radarShowAll ||
                gameplay::IsSlotMarked(slot) ||
                p.IsSprinting());
        }

        if (visible) p.radarFade = fminf(1.0f, p.radarFade + dt / kRadarFadeIn);
        else         p.radarFade = fmaxf(0.0f, p.radarFade - dt / kRadarFadeOut);

        if (p.radarFade <= 0.0f || !ptr)
            continue;

        Transform other = LOCATOR::GET_TRANSFORM(p.UserSid());
        float dx = other.Position.x - me.Position.x;
        float dz = other.Position.z - me.Position.z;

        // rotate into view space ("up" = forward)
        float relX = dx * c - dz * s;
        float relZ = dx * s + dz * c;

        // map to the radar; subtract so world-right maps to radar-right (un-mirrored)
        float radarX = RADAR_CX - relX / UNITS_PER_PX;
        float radarY = RADAR_CY - relZ / UNITS_PER_PX;

        // out of range -> clamp to the rim, kept RADAR_DOT inside the edge
        auto pos = ClampPositionRadarIconToBorder(dx, dz, relX, relZ, radarX, radarY);

        radarX = pos.first;
        radarY = pos.second;

        uint8_t a = static_cast<uint8_t>(255.0f * p.radarFade);
        auto    eSz = DebugDrawCommon::GetTextWidthHeight(kEnemyGlyph, ENEMY_BLIP_FONT);
        // Blue for teammates, red for opponents, using the shared team colours so the blip is the
        // exact same blue as the in-world teammate outline.
        DebugDrawCommon::DrawTextPrintV(radarX - eSz.x * 0.5f, radarY - eSz.y * 0.5f,
            ENEMY_BLIP_FONT,
            teammate ? _rgba(kTeamBlueR, kTeamBlueG, kTeamBlueB, a)
            : _rgba(kTeamRedR, kTeamRedG, kTeamRedB, a),
            "%s", kEnemyGlyph);
    }


    // you -- larger blip, centered on the radar center
    // render your arrow last so it's always on top
    {
        auto pSz = DebugDrawCommon::GetTextWidthHeight(kYouGlyph, YOU_BLIP_FONT);
        DebugDrawCommon::DrawTextPrintV(RADAR_CX - pSz.x * 0.5f, RADAR_CY - pSz.y * 0.5f,
            YOU_BLIP_FONT, _rgba(255, 255, 255, 255),
            "%s", kYouGlyph);
    }
}

void ResetKillFeed()
{
    s_entries.clear();
    for (int i = 0; i < kMaxNetPlayers; ++i)
        NetPlayers::BySlot(i).radarFade = 0.0f;
}
