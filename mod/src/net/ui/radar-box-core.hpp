#pragma once

#include <cmath>
#include <cstdint>

// Pure placement / visibility core of tools/ext/radar_box.c. The widget lookup,
// blip capture and HUD writes live in net/ui/radar-box.cpp. The coordinates are
// the ext's 1280 x 720 text space; the native radar supplies its own centres and
// the owner converts them into this space before feeding a blip.
namespace net::ui::radar_box::core {

    inline constexpr int   kIcons       = 12;
    inline constexpr int   kArcSteps   = 60;         // 6 degrees apart
    inline constexpr float kWidgetW    = 72.0f;      // canvas units (24 px in 1280x720)
    inline constexpr float kWidgetH    = 59.0f;
    inline constexpr float kRadarCx    = 104.0f;     // ext clan_widgets RADAR_CX
    inline constexpr float kRadarCy    = 179.0f;     // ext clan_widgets RADAR_CY
    inline constexpr float kDiscR      = 75.0f;
    inline constexpr float kPinD       = 62.5f;      // blips at/over this are on the rim
    inline constexpr float kClampD     = 60.0f;      // keep the icon inside the ring

    enum class Floor : uint8_t { Same = 0, Up = 1, Down = 2 };

    struct Blip {
        float x = 0.0f, y = 0.0f;
        Floor floor = Floor::Same;
        bool  pinned = false;
    };

    inline float Distance(float x, float y, float cx, float cy) noexcept
    {
        const float dx = x - cx, dy = y - cy;
        return std::sqrt(dx * dx + dy * dy);
    }

    // A pinned blip is one outside the radar's range. The producer knows the
    // pre-clamp distance; this helper is the fallback for clamped coordinates.
    inline bool Pinned(float x, float y) noexcept
    {
        return Distance(x, y, kRadarCx, kRadarCy) >= kPinD;
    }

    // Page widgets use the whole 3840 x 2160 canvas: x is absolute in the
    // centred 16:9 box, hud-root widgets use 1280 x 720 relative to its centre.
    inline float PivotX(float x, bool page) noexcept
    {
        return ((page ? x : x - 640.0f) * 3.0f) - kWidgetW * 0.5f;
    }

    inline float PivotY(float y) noexcept
    {
        return y * 3.0f - kWidgetH * 0.5f;
    }

    // Keep a page icon inside the disc without moving a hud-root fallback icon.
    // Returns true when it was clamped.
    inline bool ClampIcon(float& x, float& y, bool page) noexcept
    {
        if (!page) return false;
        const float dx = x - kRadarCx, dy = y - kRadarCy;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d <= kClampD || d <= 0.0f) return false;
        x = kRadarCx + dx * kClampD / d;
        y = kRadarCy + dy * kClampD / d;
        return true;
    }

    // Rim arc index: 0 = up, clockwise, one of 60 steps. `cw` mirrors the
    // layout if the GUI pak turns the other way (ext RBX_ARC_CW).
    inline int ArcIndex(float x, float y, bool cw = true) noexcept
    {
        float dx = x - kRadarCx, dy = y - kRadarCy;
        if (!cw) dx = -dx;
        float deg = std::atan2(dx, -dy) * (180.0f / 3.14159265f);
        int k = static_cast<int>(std::floor(deg * (kArcSteps / 360.0f) + 0.5f));
        k %= kArcSteps;
        if (k < 0) k += kArcSteps;
        return k;
    }

    // Floor arrows are an optional pak extra; without them every icon is plain.
    inline Floor IconState(Floor blip, bool floorWidgets) noexcept
    {
        return floorWidgets ? blip : Floor::Same;
    }

    // The ext takes the radar's own fade, or the smaller of radar and text fade
    // for the hud-root fallback. Menus / hidden HUD force zero.
    inline float IconAlpha(float radarFade, float textFade, bool page,
                           bool hidden) noexcept
    {
        if (hidden) return 0.0f;
        float a = page ? textFade
                       : (radarFade < textFade ? radarFade : textFade);
        if (a < 0.0f) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
        return a;
    }

    // Which of the three crate widgets is up for slot i.
    inline int StateSlot(Floor f) noexcept
    {
        return f == Floor::Same ? 0 : f == Floor::Up ? 1 : 2;
    }

} // namespace net::ui::radar_box::core
