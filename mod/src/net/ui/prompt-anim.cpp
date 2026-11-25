// ported from tools/ext/prompt_anim.c (build-201 working tree). The ext drew SP
// key prompts from the winmm text hook and read g_wpr / g_wpg (produced by the
// still-ext-owned interactable, melee pullout, duel pin and gift modules). This
// owner keeps the timing / geometry core and the HUD pass, uses the mod's own
// debug-draw text path and the existing hook-free window / pad samplers, and
// exposes Show() for a native producer to feed. No VA detour, no second hook.

#include "prompt-anim.hpp"

#include "boot.hpp"
#include "engine/draw.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/locale.hpp"
#include "engine/memory.hpp"
#include "engine/pad.hpp"
#include "engine/script.hpp"
#include "engine/window.hpp"
#include "log.hpp"
#include "sid/sid.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace net::ui::prompt_anim {

    namespace {

        using core::Kind;
        using core::State;

        struct Pending {
            bool     valid = false;
            Kind     kind = Kind::Text;
            char     label[48]{};
            float    progress = 0.0f, x = 640.0f, y = 300.0f;
            uint64_t frame = 0;
        };

        struct Input {
            bool pad = false;        // active device latched like the ext's PaKeyLetter
            bool keyPress = false;   // E / F rising edge
            bool padPress = false;   // melee / face-button edge from the shared pad sampler
            bool keyWas = false;
        };

        constexpr int kSlots = 2;

        Pending gPending[kSlots];
        State   gState[kSlots];
        Input   gIn;
        engine::pad::MeleeEdgeState gEdge;
        float   gDotScale = 0.0f;
        bool    gDotChecked = false;

        bool OldTexts()
        {
            return engine::flag::Present("t2r-prompt-anim-old.txt");
        }

        bool RegularSlotEnabled()
        {
            return engine::flag::Present("t2r-wp-selfdraw.txt");
        }

        // The ext drew solid lines with dense '-' strokes: one dot every ~1.4 px.
        bool DotScale()
        {
            if (gDotChecked) return gDotScale > 0.0f;
            gDotChecked = true;
            const vec2 m = DebugDrawCommon::GetTextWidthHeight("-", 1.0f);
            if (!std::isfinite(m.x) || m.x <= 0.01f || m.x >= 50.0f ||
                !std::isfinite(m.y) || m.y <= 0.01f || m.y >= 50.0f)
                return false;
            gDotScale = 3.5f / m.x;
            return true;
        }

        void Dot(float cx, float cy, float a, float lum)
        {
            if (a <= 0.0f || !DotScale()) return;
            const uint32_t alpha = static_cast<uint32_t>(
                core::Clamp01(a) * 235.0f + 0.5f);
            const uint32_t lv = static_cast<uint32_t>(
                core::Clamp01(lum) * 255.0f + 0.5f);
            if (!alpha) return;
            DebugDrawCommon::DrawTextPrintV(
                cx - gDotScale * 0.5f, cy - gDotScale * 0.5f, gDotScale,
                _rgba(lv, lv, lv, alpha), "%s", "-");
        }

        void Circle(float x, float y, float r, float a, float lum)
        {
            int n = static_cast<int>(r * 6.2832f / 1.4f);
            if (n < 24) n = 24;
            for (int i = 0; i < n; ++i) {
                const float an = static_cast<float>(i) * (6.2832f / n);
                Dot(x + r * std::cos(an), y + r * std::sin(an), a, lum);
            }
        }

        void Text(const char* text, float x, float y, float size,
                  float a, uint32_t color)
        {
            if (a <= 0.0f || !text || !*text) return;
            const char* loc = engine::locale::Tr(text);
            const vec2 m = DebugDrawCommon::GetTextWidthHeight(loc, size);
            if (!std::isfinite(m.y)) return;
            DebugDrawCommon::DrawTextPrintV(x, y - m.y * 0.5f, size,
                _rgba(static_cast<uint8_t>((color >> 16) & 0xff),
                      static_cast<uint8_t>((color >> 8) & 0xff),
                      static_cast<uint8_t>(color & 0xff),
                      static_cast<uint8_t>(core::Clamp01(a) * 255.0f + 0.5f)),
                "%s", loc);
        }

        void DrawSlot(int slot, uint64_t now)
        {
            State& s = gState[slot];
            const Pending& p = gPending[slot];
            const uint64_t frame = engine::frame::Count();
            const bool live = p.valid && frame >= p.frame && frame - p.frame <= 2;
            const bool press = live && slot == 0 &&
                (gIn.keyPress || gIn.padPress);
            if (!core::Frame(s, now, live, p.x, p.y, p.kind, p.label,
                             p.progress, press))
                return;

            const float t = core::Elapsed(s, now);
            const float fade = core::FadeOut(s, now);
            if (s.kind == Kind::Text) {
                Text(s.label, s.x,
                     s.y + core::Settle(t * 1000.0f /
                                        static_cast<float>(core::kTextMs)),
                     0.9f, core::TextAlpha(t, fade) * (160.0f / 255.0f),
                     0xA0A0A0u);
                return;
            }

            const float hs = core::kKeyHalf * core::Grow(t);
            const float oa = core::OutlineAlpha(t, fade);
            const float la = core::LetterAlpha(t, fade);
            float lum = core::LetterLum(t);
            const int nd = std::max(8, static_cast<int>(8.0f * hs / 1.4f));
            const float fill = s.kind == Kind::Hold ? s.hold : -1.0f;

            for (int i = 0; i < nd; ++i) {
                const float u = static_cast<float>(i) / nd;
                float px = 0.0f, py = 0.0f;
                core::Perimeter(s.x, s.y, hs, u, px, py);
                const bool lit = fill >= 0.0f && u < fill;
                Dot(px, py, oa * (lit ? 1.0f : 0.7f),
                    lit ? 1.0f : lum * 0.7f);
            }

            if (oa > 0.0f && fill < 0.0f) {
                const float fa = core::FaceAlpha(t, fade);
                for (float yy = -hs + 2.0f; yy <= hs - 2.0f; yy += 1.6f)
                    for (float xx = -hs + 2.0f; xx <= hs - 2.0f; xx += 1.6f)
                        Dot(s.x + xx, s.y + yy, fa, lum * 0.92f);
                lum = 0.08f;   // dark letter on the light key face
            }

            if (la > 0.0f) {
                const float a = la;
                const uint32_t lv = static_cast<uint32_t>(lum * 255.0f + 0.5f);
                Text(gIn.pad ? "Y" : "E", s.x, s.y, 1.0f, a,
                     _rgba(lv, lv, lv, 255));
            }

            if (s.kind == Kind::Wait && t > 0.12f) {
                Circle(s.x, s.y, core::RingRadius(s.hold),
                       core::WaitRingAlpha(t, fade), 0.78f);
            }
            if (s.kind == Kind::Mash) {
                float p = 0.0f, a = 0.0f;
                if (core::FlashActive(s, now, p, a))
                    Circle(s.x, s.y, core::RingRadius(p), a, 0.78f);
            }
            if (s.kind == Kind::Press || s.kind == Kind::Hold) {
                const float k = core::Clamp01((t - 0.12f) / 0.15f);
                Text(s.label, s.x + core::kKeyHalf + 10.0f,
                     s.y + core::Settle(k), 0.9f, k * fade, 0xFFFFFFu);
            }
        }

    } // namespace

    void Show(int slot, Kind kind, const char* label, float progress,
              float x, float y)
    {
        if (slot < 0 || slot >= kSlots || boot::ExtLoaded()) return;
        Pending& p = gPending[slot];
        p.valid = true;
        p.kind = kind;
        p.progress = core::Clamp01(progress);
        p.x = x < 0.0f ? 640.0f : x;
        p.y = y < 0.0f ? 300.0f : y;
        core::ClampAnchor(p.x, p.y);
        const char* src = label ? label : "";
        size_t i = 0;
        for (; i + 1 < sizeof(p.label) && src[i]; ++i) p.label[i] = src[i];
        p.label[i] = '\0';
        p.frame = engine::frame::Count() ? engine::frame::Count() : 1;
    }

    bool Project(float worldX, float worldY, float worldZ,
                 float& x, float& y)
    {
        if (boot::ExtLoaded()) return false;
        const Vector2* r = DC::Call<"world-space-to-screen-space", const Vector2>(
            0, MEMORY::ALLOC_VECTOR(Vector3{ worldX, worldY, worldZ }));
        if (!r || !std::isfinite(r->x) || !std::isfinite(r->y))
            return false;
        x = r->x;
        y = r->y;
        return true;
    }

    void Tick(uint64_t now)
    {
        if (boot::ExtLoaded()) return;
        const bool key = engine::window::KeyDown('E') ||
            engine::window::KeyDown('F') || engine::window::KeyDown('W') ||
            engine::window::KeyDown('A') || engine::window::KeyDown('S') ||
            engine::window::KeyDown('D') || engine::window::KeyDown(VK_SPACE) ||
            engine::window::KeyDown(VK_LSHIFT) ||
            engine::window::KeyDown(VK_LBUTTON) ||
            engine::window::KeyDown(VK_RBUTTON);
        const bool keyEdge = (engine::window::KeyDown('E') ||
                              engine::window::KeyDown('F')) && !gIn.keyWas;
        gIn.keyWas = key;
        gIn.keyPress = keyEdge;

        const uint64_t act = engine::pad::ActivityMs();
        const bool padSeen = act && now >= act && now - act <= 250;
        if (padSeen || engine::pad::MeleeHeld() || engine::pad::Held("interact"))
            gIn.pad = true;
        else if (key)
            gIn.pad = false;

        gIn.padPress = engine::pad::MeleeEdge(gEdge, now, false, false) > 0;
    }

    bool PressEdge() noexcept
    {
        return gIn.keyPress || gIn.padPress;
    }

    bool RegularOptIn() noexcept
    {
        return RegularSlotEnabled();
    }

    void Draw(uint64_t now)
    {
        if (boot::ExtLoaded() || OldTexts()) return;
        DrawSlot(0, now);
        if (RegularSlotEnabled()) DrawSlot(1, now);
    }

    bool Active()
    {
        if (boot::ExtLoaded() || OldTexts()) return false;
        const uint64_t f = engine::frame::Count();
        for (int i = 0; i < kSlots; ++i)
            if (gState[i].vis && (!gState[i].last ||
                                  f <= gState[i].last + 12))
                return true;
        return false;
    }

    void Reset()
    {
        for (int i = 0; i < kSlots; ++i) {
            gPending[i] = Pending{};
            gState[i] = State{};
        }
        gIn = Input{};
        gEdge = engine::pad::MeleeEdgeState{};
    }

} // namespace net::ui::prompt_anim
