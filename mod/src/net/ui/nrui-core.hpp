#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Pure layout / timing rules of the shared No Return page frame. The native
// owner in nrui.cpp supplies every widget / cursor / input seam; nothing here
// touches the engine.
//
// Layouts keep the COARSE width estimate (nr_cw / nr_est / nr_tw): that is what
// current main uses for the header, sections, chips, key boxes and tab strip.
// Where a box is fitted to its LABEL, main uses the finer per-character table
// NR_CWF (nr_cwf -> nr_twf) and the +26 prompt tail; both are ported below.

namespace net::ui::nrui::core {

    inline constexpr float kRight      = 3700.0f;   // NR_R1
    inline constexpr float kLeft       = 148.0f;    // NR_L0
    inline constexpr float kHeadPt     = 54.0f;
    inline constexpr float kRuleW0     = 1000.0f;
    inline constexpr float kHair       = 4.0f;
    inline constexpr float kTabPt      = 36.0f;
    inline constexpr float kTabY       = 270.0f;
    inline constexpr float kPromptPt   = 36.0f;
    inline constexpr float kPromptKeyPt= 24.0f;
    inline constexpr float kPromptY    = 2000.0f;
    inline constexpr float kPromptH    = 88.0f;
    inline constexpr float kPromptBoxW0= 400.0f;    // NR_PB_W0
    inline constexpr int    kNPrompt    = 6;
    inline constexpr int    kMaxTabs    = 8;
    inline constexpr float kWeekX0     = kRight - 760.0f;
    inline constexpr float kWeekW      = 760.0f;
    inline constexpr float kChipW0     = 100.0f;
    inline constexpr int    kNGlide     = 12;
    inline constexpr float kGlideTauMs = 38.0f;
    inline constexpr int    kGlideFrame = 1;
    inline constexpr int    kGlideThumb = 2;
    inline constexpr int    kClanTabs   = 5;
    inline constexpr int    kMaxXTabs   = 3;
    inline constexpr float kCharTrack  = 1.06f;

    inline constexpr const char* const kClanTabNames[kClanTabs] = {
        "OVERVIEW", "SURVIVORS", "RAIDS", "JOURNAL", "MISSIONS" };

    using Translate = const char* (*)(const char*);

    // nr_cw: space .40, lower-case (or any high byte) .62, everything else .80.
    [[nodiscard]] inline float CharWidth(unsigned char c) noexcept
    {
        if (c == ' ') return 0.40f;
        if ((c >= 'a' && c <= 'z') || c >= 0x80) return 0.62f;
        return 0.80f;
    }

    // nr_est over an already-translated string: skip <font ...> tags and UTF-8
    // continuation bytes. `s` may be null.
    [[nodiscard]] inline float EstTranslated(const char* s, float pt) noexcept
    {
        float est = 0.0f;
        bool tag = false;
        for (const unsigned char* q = reinterpret_cast<const unsigned char*>(s ? s : "");
             *q; ++q) {
            if (*q == '<') tag = true;
            if (tag) {
                if (*q == '>') tag = false;
                continue;
            }
            if ((*q & 0xC0) == 0x80) continue;
            est += CharWidth(*q);
        }
        return est * pt;
    }

    [[nodiscard]] inline float Est(const char* t, float pt, Translate tr = nullptr) noexcept
    {
        const char* src = t ? t : "";
        const char* s = tr ? tr(src) : src;
        return EstTranslated(s ? s : src, pt);
    }

    [[nodiscard]] inline float Tw(const char* t, float pt, Translate tr = nullptr) noexcept
    {
        return Est(t, pt, tr) * kCharTrack;
    }

    // Current main's finer advance widths (nrui.c NR_CWF[95]): the game's menu
    // face (DINPro Medium) for ' ' .. '~', in nr_cw units; a high byte is 0.66.
    // Used only where a box is fitted to its text (prompt labels); nr_cw /
    // nr_est / nr_tw stay authoritative for the rest of the layout.
    inline constexpr float kFineCharWidth[95] = {
        0.308f, 0.427f, 0.570f, 0.863f, 0.781f, 1.082f, 0.938f, 0.330f, 0.407f, 0.407f, 0.623f, 0.685f,
        0.349f, 0.547f, 0.357f, 0.488f, 0.685f, 0.685f, 0.685f, 0.685f, 0.685f, 0.685f, 0.685f, 0.685f,
        0.685f, 0.685f, 0.389f, 0.389f, 0.685f, 0.685f, 0.685f, 0.657f, 0.946f, 0.803f, 0.852f, 0.807f,
        0.856f, 0.775f, 0.752f, 0.834f, 0.888f, 0.378f, 0.650f, 0.845f, 0.737f, 1.045f, 0.928f, 0.830f,
        0.808f, 0.830f, 0.842f, 0.758f, 0.719f, 0.867f, 0.725f, 1.119f, 0.743f, 0.699f, 0.709f, 0.446f,
        0.488f, 0.446f, 0.734f, 0.734f, 0.642f, 0.681f, 0.710f, 0.623f, 0.710f, 0.692f, 0.421f, 0.705f,
        0.727f, 0.339f, 0.339f, 0.704f, 0.400f, 1.127f, 0.730f, 0.690f, 0.710f, 0.710f, 0.564f, 0.641f,
        0.439f, 0.730f, 0.605f, 0.955f, 0.646f, 0.601f, 0.603f, 0.523f, 0.434f, 0.523f, 0.737f,
    };

    [[nodiscard]] inline float FineCharWidth(unsigned char c) noexcept
    {
        if (c >= 32 && c < 127) return kFineCharWidth[c - 32];
        return c >= 0x80 ? 0.66f : 0.0f;
    }

    // nr_twf: nr_tw with nr_cwf (translated, colour tags skipped, UTF-8
    // continuation bytes skipped, tracking x 1.06).
    [[nodiscard]] inline float TwFine(const char* t, float pt,
                                      Translate tr = nullptr) noexcept
    {
        const char* src = t ? t : "";
        const char* s = tr ? tr(src) : src;
        float est = 0.0f;
        bool tag = false;
        for (const unsigned char* q = reinterpret_cast<const unsigned char*>(s ? s : src);
             *q; ++q) {
            if (*q == '<') tag = true;
            if (tag) {
                if (*q == '>') tag = false;
                continue;
            }
            if ((*q & 0xC0) == 0x80) continue;
            est += FineCharWidth(*q);
        }
        return est * pt * kCharTrack;
    }

    // nr_rule: fills are built 1000 wide, left-middle pivot; never thinner than 2.
    [[nodiscard]] inline float RuleScale(float w) noexcept
    {
        return (w < 2.0f ? 2.0f : w) / kRuleW0;
    }

    // nr_bar: 0..1 clamp, hidden at <= 0.004, x0..x1 scaled by the same 1000 template.
    [[nodiscard]] inline bool BarVisible(float frac) noexcept
    {
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        return frac > 0.004f;
    }

    [[nodiscard]] inline float BarScale(float x0, float x1, float frac) noexcept
    {
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        return (x1 - x0) * frac / kRuleW0;
    }

    // nr_fit: shrink to fit a box, never below 0.6.
    [[nodiscard]] inline float FitScale(float estimate, float box) noexcept
    {
        float s = estimate > box && estimate > 0.0f ? box / estimate : 1.0f;
        return s < 0.6f ? 0.6f : s;
    }

    struct HeaderPos {
        float divX = 0.0f;
        float pageX = 0.0f;
        float ruleX = 0.0f;
        float ruleW = 0.0f;
    };

    // nr_header: parent text is left at NR_L0; the divider, page and rule follow
    // its measured (translated) width with 52-unit gaps.
    [[nodiscard]] inline HeaderPos HeaderPositions(float parentWidth, float pageWidth) noexcept
    {
        HeaderPos p;
        p.divX = kLeft + parentWidth + 52.0f;
        p.pageX = p.divX + 52.0f;
        p.ruleX = p.pageX + pageWidth + 52.0f;
        p.ruleW = kRight - p.ruleX;
        return p;
    }

    inline void WeekText(int week, int day, int survived,
                         char* dayOut, size_t dayCap,
                         char* svOut, size_t svCap) noexcept
    {
        if (dayOut && dayCap) std::snprintf(dayOut, dayCap, "WEEK %d: DAY %d", week, day);
        if (svOut && svCap) std::snprintf(svOut, svCap, "DAYS SURVIVED %d | 84", survived);
    }

    [[nodiscard]] inline float WeekFrac(int survived) noexcept
    {
        float f = static_cast<float>(survived) / 84.0f;
        return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    }

    [[nodiscard]] inline float ChipWidth(const char* t, float pt,
                                         Translate tr = nullptr) noexcept
    {
        return Tw(t, pt, tr) + 32.0f;
    }

    struct PromptKey {
        char  k1[48]{};
        char  k2[48]{};
        bool  glyph = false;
        float kw = 0.0f;
    };

    // nr_prompts' key half. A "menu:*" key is an engine glyph (one id, or two
    // joined by '+'); anything else is plain text in a white box. The pad key
    // is used only in pad mode.
    [[nodiscard]] inline PromptKey PromptKeyOf(const char* kb, const char* pad,
                                               bool padMode, Translate tr = nullptr) noexcept
    {
        const char* key = padMode && pad ? pad : (kb ? kb : "");
        PromptKey p;
        p.glyph = std::strncmp(key, "menu:", 5) == 0;
        if (p.glyph) {
            const char* plus = std::strchr(key, '+');
            if (plus) {
                const size_t n = static_cast<size_t>(plus - key);
                if (n < sizeof p.k1) {
                    std::memcpy(p.k1, key, n);
                    p.k1[n] = 0;
                }
                std::snprintf(p.k2, sizeof p.k2, "%s", plus + 1);
            } else {
                std::snprintf(p.k1, sizeof p.k1, "%s", key);
            }
            p.kw = p.k2[0] ? 136.0f : 64.0f;
        } else {
            p.kw = Tw(key, kPromptKeyPt, tr) + 28.0f;
            if (p.kw < 60.0f) p.kw = 60.0f;
        }
        return p;
    }

    struct PromptBox {
        PromptKey key;
        float lw = 0.0f;
        float tot = 0.0f;
        float x0 = 0.0f;
        float x1 = 0.0f;
        float labelX = 0.0f;
        float alpha = 0.0f;
    };

    // One box placed with its RIGHT edge at x (boxes are laid out right to left).
    // Main's tail: 18 + kw + 22 + lw(fine) + 26; the +34 with a coarse label
    // was the older, roomier estimate.
    [[nodiscard]] inline PromptBox PromptBoxAt(const PromptKey& key, const char* label,
                                               float x, bool on, Translate tr = nullptr) noexcept
    {
        PromptBox b;
        b.key = key;
        b.lw = TwFine(label, kPromptPt, tr);
        b.tot = 18.0f + key.kw + 22.0f + b.lw + 26.0f;
        b.x0 = x - b.tot;
        b.x1 = x;
        b.labelX = 18.0f + key.kw + 22.0f;
        b.alpha = on ? 70.0f / 255.0f : 30.0f / 255.0f;
        return b;
    }

    // nr_prompt_hover: boxes[k] holds declaration index n-1-k; -1 = no hit.
    [[nodiscard]] inline int PromptHover(float cx, float cy, const PromptBox* boxes,
                                         int n) noexcept
    {
        if (!boxes) return -1;
        if (cy < kPromptY - kPromptH / 2.0f - 6.0f
            || cy > kPromptY + kPromptH / 2.0f + 6.0f) return -1;
        for (int k = 0; k < n; ++k)
            if (cx >= boxes[k].x0 && cx <= boxes[k].x1) return n - 1 - k;
        return -1;
    }

    struct SectionLine {
        float xa = 0.0f;
        float xb = 0.0f;
        float width = 0.0f;
    };

    // nr_section: rule between title and optional right text, each with a 40 gap.
    [[nodiscard]] inline SectionLine SectionRule(float x0, float x1, float titleWidth,
                                                 float rightWidth) noexcept
    {
        SectionLine l;
        l.xa = x0 + titleWidth + 40.0f;
        l.xb = x1 - rightWidth;
        l.width = l.xb - l.xa;
        return l;
    }

    // nr_frame_draw: the four hairlines of a focus frame. k = 0 T, 1 B, 2 L, 3 R.
    inline void FramePiece(int k, float x, float y, float w, float h,
                           float& px, float& py, float& sx, float& sy) noexcept
    {
        if (k == 0) {
            px = x; py = y + 2.0f; sx = w / 100.0f; sy = 1.0f;
        } else if (k == 1) {
            px = x; py = y + h - 2.0f; sx = w / 100.0f; sy = 1.0f;
        } else {
            px = k == 2 ? x : x + w - 4.0f; py = y; sx = 1.0f; sy = h / 100.0f;
        }
    }

    // nr_scroll_v's thumb rectangle: out = x, y, length, built height.
    [[nodiscard]] inline bool ThumbRect(float x, float y0, float y1, float h0,
                                        int first, int shown, int n,
                                        float out[4]) noexcept
    {
        if (!out || n <= 0 || n <= shown) return false;
        float a = static_cast<float>(first) / static_cast<float>(n);
        float b = static_cast<float>(first + shown) / static_cast<float>(n);
        float len = (y1 - y0) * (b - a);
        if (len < 8.0f) len = 8.0f;
        out[0] = x - 2.0f;
        out[1] = y0 + (y1 - y0) * a;
        out[2] = len;
        out[3] = h0;
        return true;
    }

    [[nodiscard]] inline float GlideStep(float dtMs) noexcept
    {
        float dt = dtMs < 0.0f ? 0.0f : (dtMs > 100.0f ? 100.0f : dtMs);
        return 1.0f - std::exp(-dt / kGlideTauMs);
    }

    // The ext finishes a glide and snaps when the remainder is within 0.5 units.
    [[nodiscard]] inline bool GlideDone(float current, float target) noexcept
    {
        float d = target - current;
        return d <= 0.5f && d >= -0.5f;
    }

    struct TabLayout {
        float x[kMaxTabs]{};
        float w[kMaxTabs]{};
        int   n = 0;
    };

    inline void BuildTabs(TabLayout& t, const char* const* labels, int n,
                          Translate tr = nullptr) noexcept
    {
        t = TabLayout{};
        if (n > kMaxTabs) n = kMaxTabs;
        t.n = n < 0 ? 0 : n;
        float x = kLeft + 108.0f;
        for (int k = 0; k < t.n; ++k) {
            t.w[k] = Tw(labels ? labels[k] : "", kTabPt, tr) + 8.0f;
            t.x[k] = x;
            x += t.w[k] + 80.0f;
        }
    }

    // nr_clan_tab_hover: Q glyph -2, tabs 0.., E glyph -3, none -1.
    [[nodiscard]] inline int TabHover(float cx, float cy, const TabLayout& t) noexcept
    {
        if (cy < kTabY - 60.0f || cy > kTabY + 60.0f) return -1;
        if (cx >= kLeft - 20.0f && cx <= kLeft + 80.0f) return -2;
        for (int k = 0; k < t.n; ++k)
            if (cx >= t.x[k] - 30.0f && cx <= t.x[k] + t.w[k] + 30.0f) return k;
        if (t.n > 0) {
            float ex = t.x[t.n - 1] + t.w[t.n - 1] + 28.0f;
            if (cx >= ex - 20.0f && cx <= ex + 80.0f) return -3;
        }
        return -1;
    }

    [[nodiscard]] inline int WrapTab(int tab, int n) noexcept
    {
        if (n <= 0) return 0;
        return (tab % n + n) % n;
    }

} // namespace net::ui::nrui::core
