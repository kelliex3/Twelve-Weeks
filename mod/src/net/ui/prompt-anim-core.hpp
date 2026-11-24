#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

// Pure timing / geometry core of tools/ext/prompt_anim.c (SP-style key prompts).
// The widget / HUD-draw / input side lives in net/ui/prompt-anim.cpp. Everything
// here is engine-free so the ext's constants and curves can be tested standalone.
namespace net::ui::prompt_anim::core {

    inline constexpr float    kKeyHalf      = 17.0f;
    inline constexpr float    kRingStart    = 1.5f;
    inline constexpr float    kFlashS       = 0.18f;
    inline constexpr uint64_t kFadeOutMs    = 100;
    inline constexpr uint64_t kGrowMs       = 120;
    inline constexpr uint64_t kFaceMs       = 230;
    inline constexpr uint64_t kTextMs       = 150;
    inline constexpr uint64_t kRingInMs     = 100;

    enum class Kind : uint8_t {
        Text  = 0,   // plain grey label
        Wait  = 1,   // key + contracting timer ring (countdown progress)
        Mash  = 2,   // key + per-press ring flash
        Press = 3,   // key + label
        Hold  = 4,   // key border fills clockwise with progress
    };

    // Mirrors the ext's g_pa[slot] record. `t0` starts the appear animation,
    // `last` is the most recent active frame (the fade-out clock), `flash` the
    // last mash press. `label` is the localised text the producer supplied.
    struct State {
        bool     vis = false;      // still being drawn (including the fade-out)
        bool     active = false;   // fed this frame; drives the fade-out clock
        Kind     kind = Kind::Text;
        uint64_t t0 = 0, last = 0, flash = 0;
        float    x = 0.0f, y = 0.0f, hold = 0.0f;
        char     label[48]{};
    };

    inline float Clamp01(float v) noexcept
    {
        return v <= 0.0f ? 0.0f : v >= 1.0f ? 1.0f : v;
    }

    inline float Elapsed(const State& s, uint64_t now) noexcept
    {
        return now <= s.t0 ? 0.0f : static_cast<float>(now - s.t0) * 0.001f;
    }

    // Multiplier applied to every drawn alpha while the prompt is leaving.
    inline float FadeOut(const State& s, uint64_t now) noexcept
    {
        if (s.active || !s.last || now <= s.last) return 1.0f;
        const uint64_t dt = now - s.last;
        return dt >= kFadeOutMs ? 0.0f : 1.0f - static_cast<float>(dt) / 100.0f;
    }

    // One frame of one slot. Returns false when the slot has nothing to draw
    // (never shown, or fully faded out). A kind / label change restarts t0,
    // exactly like the ext's PaFrame.
    inline bool Frame(State& s, uint64_t now, bool active, float x, float y,
                      Kind kind, const char* label, float prog,
                      bool pressEdge) noexcept
    {
        s.active = active;
        if (active) {
            const bool relabel = kind == Kind::Text &&
                std::strcmp(label ? label : "", s.label) != 0;
            if (!s.vis || s.kind != kind || relabel) {
                s.vis = true;
                s.kind = kind;
                s.t0 = now;
                s.flash = 0;
            }
            s.last = now;
            s.x = x;
            s.y = y;
            s.hold = Clamp01(prog);
            const char* src = label ? label : "";
            size_t i = 0;
            for (; i + 1 < sizeof(s.label) && src[i]; ++i) s.label[i] = src[i];
            s.label[i] = '\0';
            if (kind == Kind::Mash && pressEdge) s.flash = now;
        }
        else if (!s.vis || now <= s.last || now - s.last > kFadeOutMs) {
            s.vis = false;
            return false;
        }

        const float fade = FadeOut(s, now);
        if (fade <= 0.0f) {
            s.vis = false;
            return false;
        }
        return true;
    }

    // SP key appear: outline grows, letter fades to white.
    inline float Grow(float t) noexcept
    {
        const float g = Clamp01(t * 1000.0f / static_cast<float>(kGrowMs));
        return 0.35f + 0.65f * g * (2.0f - g);
    }

    inline float OutlineAlpha(float t, float fade) noexcept
    {
        return Clamp01(t * 1000.0f / 50.0f) * fade;
    }

    inline float LetterAlpha(float t, float fade) noexcept
    {
        const float u = (t - 0.12f) / 0.08f;
        return Clamp01(u) * fade;
    }

    inline float LetterLum(float t) noexcept
    {
        return 0.55f + 0.45f * Clamp01((t - 0.12f) / 0.23f);
    }

    inline float FaceAlpha(float t, float fade) noexcept
    {
        return Clamp01((t - 0.12f) / 0.23f) * fade *
               (0.25f + 0.65f * Clamp01((t - 0.12f) / 0.23f));
    }

    // Small upward settle for a label (ext: 4 * (1 - k*(2-k))).
    inline float Settle(float k) noexcept
    {
        const float u = Clamp01(k);
        return 4.0f * (1.0f - u * (2.0f - u));
    }

    inline float TextAlpha(float t, float fade) noexcept
    {
        return Clamp01(t * 1000.0f / static_cast<float>(kTextMs)) * fade;
    }

    // A point on the square key border, u in [0,1) clockwise from top middle.
    inline void Perimeter(float cx, float cy, float s, float u,
                          float& px, float& py) noexcept
    {
        u -= static_cast<float>(static_cast<int>(u));
        if (u < 0.0f) u += 1.0f;
        float d = u * 8.0f * s;
        const float e = 2.0f * s;
        if (d < s) { px = cx + d; py = cy - s; return; }
        d -= s;
        if (d < e) { px = cx + s; py = cy - s + d; return; }
        d -= e;
        if (d < e) { px = cx + s - d; py = cy + s; return; }
        d -= e;
        if (d < e) { px = cx - s; py = cy + s - d; return; }
        d -= e;
        px = cx - s + d;
        py = cy - s;
    }

    // Wait / mash ring radius: 1.5x the key at progress 0, on the border at 1.
    inline float RingRadius(float progress) noexcept
    {
        return (kKeyHalf + 3.0f) *
               (kRingStart - (kRingStart - 1.0f) * Clamp01(progress));
    }

    inline float WaitRingAlpha(float t, float fade) noexcept
    {
        return Clamp01((t - 0.12f) * 1000.0f /
                       static_cast<float>(kRingInMs)) * fade * 0.8f;
    }

    inline bool FlashActive(const State& s, uint64_t now,
                            float& p, float& a) noexcept
    {
        if (!s.flash || now <= s.flash ||
            now - s.flash >= static_cast<uint64_t>(kFlashS * 1000.0f))
            return false;
        p = static_cast<float>(now - s.flash) / (kFlashS * 1000.0f);
        a = (1.0f - p) * FadeOut(s, now) * 0.85f;
        return true;
    }

    // The ext's clamped fallback anchor (640/300, with the same 60..1100 /
    // 80..640 box). Producers with a real world position supply x/y directly.
    inline void ClampAnchor(float& x, float& y) noexcept
    {
        if (x < 60.0f) x = 60.0f;
        if (x > 1100.0f) x = 1100.0f;
        if (y < 80.0f) y = 80.0f;
        if (y > 640.0f) y = 640.0f;
    }

} // namespace net::ui::prompt_anim::core
