// Native owner of the shared No Return frame. Layout and width rules (coarse
// Tw for keys / headers / sections, fine TwFine for prompt labels, +26 tail)
// come from nrui-core.hpp; this module supplies the widget / input seams.

#include "nrui.hpp"

#include "engine/flag.hpp"
#include "engine/locale.hpp"
#include "engine/menu-input.hpp"
#include "engine/widget.hpp"
#include "engine/window.hpp"

#include <cstdio>
#include <cstring>

namespace net::ui::nrui {

    namespace {

        using core::Translate;

        Translate Tr() { return &engine::locale::Tr; }

        void Vis(const char* path, bool on)
        {
            const uint64_t w = engine::widget::Page(path);
            if (w) engine::widget::Visible(w, on);
        }
        void Txt(const char* path, const char* text)
        {
            const uint64_t w = engine::widget::Page(path);
            if (w) engine::widget::SetText(w, text);
        }
        void Img(const char* path, const char* image)
        {
            const uint64_t w = engine::widget::Page(path);
            if (w) engine::widget::SetImage(w, image);
        }
        void Piv(const char* path, float x, float y)
        {
            const uint64_t w = engine::widget::Page(path);
            if (w) engine::widget::Pivot(w, x, y);
        }
        void Scale(const char* path, float sx, float sy)
        {
            const uint64_t w = engine::widget::Page(path);
            if (w) engine::widget::Scale(w, sx, sy);
        }
        void Alpha(const char* path, float a)
        {
            const uint64_t w = engine::widget::Page(path);
            if (w) engine::widget::Alpha(w, a);
        }

        // nr_rule: the fill was built 1000 wide with a left-middle pivot.
        void Rule(const char* path, float x, float y, float w)
        {
            const uint64_t r = engine::widget::Page(path);
            if (!r) return;
            engine::widget::Pivot(r, x, y);
            engine::widget::Scale(r, core::RuleScale(w), 1.0f);
        }

        struct Glide {
            char     base[72]{};
            int      kind = 0;
            uint64_t h = 0;
            float    c[4]{};
            float    t[4]{};
        };

        Glide   g_gl[core::kNGlide];
        bool    g_moving = false;
        uint64_t g_lastTick = 0;

        void FrameDraw(const char* base, const float* v)
        {
            static const char S[4] = {'T', 'B', 'L', 'R'};
            char p[128];
            for (int k = 0; k < 4; ++k) {
                std::snprintf(p, sizeof p, "%s%c", base, S[k]);
                const uint64_t r = engine::widget::Page(p);
                if (!r) continue;
                engine::widget::Visible(r, true);
                float px = 0.0f, py = 0.0f, sx = 1.0f, sy = 1.0f;
                core::FramePiece(k, v[0], v[1], v[2], v[3], px, py, sx, sy);
                engine::widget::Pivot(r, px, py);
                engine::widget::Scale(r, sx, sy);
            }
        }

        void ThumbDraw(const char* base, const float* v)
        {
            char p[128];
            std::snprintf(p, sizeof p, "%sH", base);
            const uint64_t r = engine::widget::Page(p);
            if (!r) return;
            engine::widget::Pivot(r, v[0], v[1]);
            engine::widget::Scale(r, 1.0f,
                v[2] / (v[3] > 1.0f ? v[3] : 1.0f));
        }

        void GlideDrop(const char* base)
        {
            for (Glide& g : g_gl)
                if (g.base[0] && std::strcmp(g.base, base) == 0) g.base[0] = 0;
        }

        // A new base, a different widget handle, a different kind or the
        // no-animation sidecar snaps; otherwise the current value eases.
        Glide* GlideSet(const char* base, int kind, uint64_t h, const float* t)
        {
            Glide* g = nullptr;
            Glide* free = nullptr;
            for (Glide& slot : g_gl) {
                if (slot.base[0] && std::strcmp(slot.base, base) == 0) { g = &slot; break; }
                if (!free && !slot.base[0]) free = &slot;
            }
            bool snap = false;
            if (!g) {
                g = free ? free : &g_gl[0];
                std::snprintf(g->base, sizeof g->base, "%s", base);
                snap = true;
            }
            if (g->h != h || g->kind != kind
                || engine::flag::Present("t2r-gu-noanim.txt")) snap = true;
            g->h = h;
            g->kind = kind;
            for (int k = 0; k < 4; ++k) {
                g->t[k] = t[k];
                if (snap) g->c[k] = t[k];
            }
            if (!snap) g_moving = true;
            return g;
        }

        core::PromptBox g_boxes[core::kNPrompt];
        int             g_nbox = 0;

        struct XTab {
            const char* label = nullptr;
            const void* key = nullptr;
            AvailFn     avail = nullptr;
            OpenFn      open = nullptr;
            PageFn      page = nullptr;
            bool        used = false;
        };
        XTab g_xtab[core::kMaxXTabs];
        void (*g_clanGo)(int, const char*) = nullptr;
        core::TabLayout g_tabs;

        bool XTabOn(int i)
        {
            if (i < 0 || i >= core::kMaxXTabs || !g_xtab[i].used) return false;
            return !g_xtab[i].avail || g_xtab[i].avail();
        }

        // tab index >= 5 -> registry entry, skipping unavailable pages.
        int XTabOf(int tab)
        {
            int t = core::kClanTabs;
            for (int i = 0; i < core::kMaxXTabs; ++i) {
                if (!XTabOn(i)) continue;
                if (t == tab) return i;
                ++t;
            }
            return -1;
        }

    } // namespace

    // ---------------------------------------------------------------- header / week

    void Header(const char* parent, const char* page)
    {
        const char* p = parent ? parent : "";
        const char* q = page ? page : "";
        Txt("anim-group.hdPar", p);
        core::HeaderPos h = core::HeaderPositions(core::Tw(p, core::kHeadPt, Tr()),
                                                 core::Tw(q, core::kHeadPt, Tr()));
        Piv("anim-group.hdDiv", h.divX, 168.0f - 34.0f);
        Piv("anim-group.hdPage", h.pageX, 168.0f);
        Txt("anim-group.hdPage", q);
        Rule("anim-group.hdRule", h.ruleX, 168.0f, h.ruleW);
    }

    void Week(int week, int day, int survived)
    {
        char b[96];
        core::WeekText(week, day, survived, b, sizeof b, nullptr, 0);
        Txt("anim-group.wkDay", b);
        core::WeekText(week, day, survived, nullptr, 0, b, sizeof b);
        Txt("anim-group.wkSv", b);

        const float frac = core::WeekFrac(survived);
        const uint64_t r = engine::widget::Page("anim-group.wkBarF");
        if (!r) return;
        engine::widget::Visible(r, core::BarVisible(frac));
        if (!core::BarVisible(frac)) return;
        engine::widget::Pivot(r, core::kWeekX0, 352.0f + 3.0f);
        engine::widget::Scale(r, core::BarScale(core::kWeekX0,
            core::kWeekX0 + core::kWeekW, frac), 1.0f);
    }

    void WeekVisible(bool on)
    {
        Vis("anim-group.wkDay", on);
        Vis("anim-group.wkSv", on);
        Vis("anim-group.wkBarT", on);
        Vis("anim-group.wkBarF", on);
        char p[48];
        for (int k = 1; k < 12; ++k) {
            std::snprintf(p, sizeof p, "anim-group.wkTk%d", k);
            Vis(p, on);
        }
    }

    void Section(const char* base, float x0, float x1, float y,
                 const char* title, const char* right, float titlePt)
    {
        char p[160];
        std::snprintf(p, sizeof p, "%sTx", base);
        Txt(p, title ? title : "");
        std::snprintf(p, sizeof p, "%sRt", base);
        Txt(p, right ? right : "");
        const float rw = right && right[0] ? core::Tw(right, 33.0f, Tr()) : 0.0f;
        core::SectionLine l = core::SectionRule(x0, x1,
            core::Tw(title, titlePt, Tr()), rw);
        std::snprintf(p, sizeof p, "%sRl", base);
        Rule(p, l.xa, y, l.width);
    }

    float Chip(const char* base, const char* text, float x, float right, float pt)
    {
        char p[160];
        float px = 0.0f, py = 0.0f;
        const float w = core::ChipWidth(text, pt, Tr());
        if (right > 0.0f) x = right - w;
        std::snprintf(p, sizeof p, "%sTx", base);
        Txt(p, text ? text : "");
        uint64_t r = engine::widget::Page(p);
        if (r && engine::widget::PivotGet(r, &px, &py))
            engine::widget::Pivot(r, x + 16.0f, py);
        std::snprintf(p, sizeof p, "%sBg", base);
        r = engine::widget::Page(p);
        if (r && engine::widget::PivotGet(r, &px, &py)) {
            engine::widget::Pivot(r, x, py);
            engine::widget::Scale(r, w / core::kChipW0, 1.0f);
        }
        return w;
    }

    // ---------------------------------------------------------------- prompts

    void Prompts(const Prompt* items, int n)
    {
        if (n > core::kNPrompt) n = core::kNPrompt;
        if (n < 0) n = 0;
        g_nbox = n;
        float x = core::kRight;
        const bool pad = engine::menu_input::PadMode();
        static const char* const S4[4] = {"T", "B", "L", "R"};
        char p[128];
        for (int k = 0; k < core::kNPrompt; ++k) {
            std::snprintf(p, sizeof p, "anim-group.pbar.pb%d", k);
            if (k >= n) {
                Vis(p, false);
                continue;
            }
            const Prompt& q = items[n - 1 - k];
            core::PromptKey key = core::PromptKeyOf(q.kb, q.pad, pad, Tr());
            core::PromptBox b = core::PromptBoxAt(key, q.label, x, q.on, Tr());
            g_boxes[k] = b;
            Vis(p, true);
            Piv(p, b.x0, core::kPromptY - core::kPromptH / 2.0f);
            for (int s = 0; s < 4; ++s) {
                std::snprintf(p, sizeof p, "anim-group.pbar.pb%d.pb%d%s", k, k, S4[s]);
                Alpha(p, b.alpha);
                if (s < 2) Scale(p, b.tot / core::kPromptBoxW0, 1.0f);
                if (s == 3) Piv(p, b.tot - core::kHair, 0.0f);
            }
            std::snprintf(p, sizeof p, "anim-group.pbar.pb%d.pb%dKg", k, k);
            Vis(p, key.glyph);
            if (key.glyph) {
                Img(p, key.k1);
                Alpha(p, q.on ? 1.0f : 0.4f);
            }
            std::snprintf(p, sizeof p, "anim-group.pbar.pb%d.pb%dKg2", k, k);
            Vis(p, key.glyph && key.k2[0]);
            if (key.glyph && key.k2[0]) {
                Img(p, key.k2);
                Alpha(p, q.on ? 1.0f : 0.4f);
            }
            std::snprintf(p, sizeof p, "anim-group.pbar.pb%d.pb%dKb", k, k);
            Vis(p, !key.glyph);
            if (!key.glyph) {
                Scale(p, key.kw / 100.0f, 1.0f);
                Alpha(p, q.on ? 1.0f : 0.4f);
            }
            std::snprintf(p, sizeof p, "anim-group.pbar.pb%d.pb%dKt", k, k);
            Vis(p, !key.glyph);
            if (!key.glyph) {
                Txt(p, key.glyph ? "" : (pad && q.pad ? q.pad : q.kb ? q.kb : ""));
                Piv(p, 18.0f + key.kw / 2.0f - 50.0f, 16.0f);
            }
            std::snprintf(p, sizeof p, "anim-group.pbar.pb%d.pb%dTx", k, k);
            char lb[128];
            std::snprintf(lb, sizeof lb, q.on ? "%s" : "<font color=\"#585858\">%s</font>",
                          q.label ? q.label : "");
            Txt(p, lb);
            Piv(p, b.labelX, core::kPromptH / 2.0f);
            x -= b.tot + 28.0f;
        }
    }

    bool HoverDisabled()
    {
        return engine::menu_input::PadMode();
    }

    int PromptHover()
    {
        if (HoverDisabled()) return -1;
        float cx = 0.0f, cy = 0.0f;
        if (!engine::window::CursorCanvas(&cx, &cy)) return -1;
        return core::PromptHover(cx, cy, g_boxes, g_nbox);
    }

    // ---------------------------------------------------------------- frames / scroll

    void FrameAt(const char* base, float x, float y, float w, float h, bool on)
    {
        static const char S[4] = {'T', 'B', 'L', 'R'};
        char p[128];
        if (!on) {
            GlideDrop(base);
            for (int k = 0; k < 4; ++k) {
                std::snprintf(p, sizeof p, "%s%c", base, S[k]);
                Vis(p, false);
            }
            return;
        }
        std::snprintf(p, sizeof p, "%sT", base);
        const uint64_t r = engine::widget::Page(p);
        if (!r) return;
        const float t[4] = {x, y, w, h};
        Glide* g = GlideSet(base, core::kGlideFrame, r, t);
        FrameDraw(base, g->c);
    }

    void ScrollV(const char* base, float x, float y0, float y1, float h0,
                 int first, int shown, int n)
    {
        const bool on = n > shown && n > 0;
        char p[128];
        std::snprintf(p, sizeof p, "%sT", base);
        Vis(p, on);
        std::snprintf(p, sizeof p, "%sH", base);
        Vis(p, on);
        if (!on) {
            GlideDrop(base);
            return;
        }
        const uint64_t r = engine::widget::Page(p);
        if (!r) return;
        float t[4]{};
        if (!core::ThumbRect(x, y0, y1, h0, first, shown, n, t)) return;
        Glide* g = GlideSet(base, core::kGlideThumb, r, t);
        ThumbDraw(base, g->c);
    }

    void Tick(uint64_t now)
    {
        const float dt = g_lastTick ? static_cast<float>(now - g_lastTick) : 16.0f;
        g_lastTick = now;
        if (!g_moving) return;
        const float k = core::GlideStep(dt);
        bool moving = false;
        for (Glide& g : g_gl) {
            if (!g.base[0]) continue;
            char p[96];
            const char leaf = g.kind == core::kGlideFrame ? 'T' : 'H';
            std::snprintf(p, sizeof p, "%s%c", g.base, leaf);
            if (engine::widget::Page(p) != g.h) {
                g.base[0] = 0;
                continue;
            }
            bool done = true;
            for (int j = 0; j < 4; ++j) {
                const float d = g.t[j] - g.c[j];
                if (core::GlideDone(g.c[j], g.t[j])) g.c[j] = g.t[j];
                else { g.c[j] += d * k; done = false; }
            }
            if (g.kind == core::kGlideFrame) FrameDraw(g.base, g.c);
            else ThumbDraw(g.base, g.c);
            if (!done) moving = true;
        }
        g_moving = moving;
    }

    // ---------------------------------------------------------------- clan tabs

    int AddXTab(const char* label, const void* key, AvailFn avail,
                OpenFn open, PageFn page)
    {
        for (int i = 0; i < core::kMaxXTabs; ++i)
            if (g_xtab[i].used && (g_xtab[i].key == key || g_xtab[i].open == open))
                return core::kClanTabs + i;
        for (int i = 0; i < core::kMaxXTabs; ++i) {
            if (g_xtab[i].used) continue;
            g_xtab[i] = XTab{label ? label : "", key, avail, open, page, true};
            return core::kClanTabs + i;
        }
        return -1;
    }

    int TabCount()
    {
        int n = core::kClanTabs;
        for (int i = 0; i < core::kMaxXTabs; ++i) if (XTabOn(i)) ++n;
        return n;
    }

    const char* TabLabel(int tab)
    {
        if (tab >= 0 && tab < core::kClanTabs) return core::kClanTabNames[tab];
        const int i = XTabOf(tab);
        return i >= 0 ? g_xtab[i].label : "";
    }

    int TabOfPage(uint64_t top)
    {
        if (!top) return -1;
        int t = core::kClanTabs;
        for (int i = 0; i < core::kMaxXTabs; ++i) {
            if (!XTabOn(i)) continue;
            if (g_xtab[i].page && g_xtab[i].page() == top) return t;
            ++t;
        }
        return -1;
    }

    bool Route(uint64_t top)
    {
        if (!top) return false;
        for (int i = 0; i < core::kMaxXTabs; ++i)
            if (g_xtab[i].page && g_xtab[i].page() == top) return true;
        return false;
    }

    void SetClanGo(void (*go)(int, const char*)) { g_clanGo = go; }

    void Go(int tab, const char* why)
    {
        const int n = TabCount();
        tab = core::WrapTab(tab, n);
        if (tab < core::kClanTabs) {
            if (g_clanGo) g_clanGo(tab, why);
            return;
        }
        const int i = XTabOf(tab);
        if (i >= 0 && g_xtab[i].open) g_xtab[i].open(why);
    }

    void DrawTabs(int active, int hover)
    {
        const int want = TabCount();
        const int n = want < core::kMaxTabs ? want : core::kMaxTabs;
        const char* labels[core::kMaxTabs]{};
        for (int k = 0; k < n; ++k) labels[k] = TabLabel(k);
        core::BuildTabs(g_tabs, labels, n, Tr());

        char p[96];
        for (int k = 0; k < core::kMaxTabs; ++k) {
            std::snprintf(p, sizeof p, "anim-group.tabs.tb%d", k);
            Vis(p, k < n);
            std::snprintf(p, sizeof p, "anim-group.tabs.tbHit%d", k);
            Vis(p, k < n);
            if (k >= n) continue;
            char tx[128];
            std::snprintf(tx, sizeof tx,
                          k == active || k == hover ? "%s"
                                                   : "<font color=\"#8C8C8C\">%s</font>",
                          labels[k]);
            std::snprintf(p, sizeof p, "anim-group.tabs.tb%d", k);
            Txt(p, tx);
            Piv(p, g_tabs.x[k], core::kTabY);
            std::snprintf(p, sizeof p, "anim-group.tabs.tbHit%d", k);
            Piv(p, g_tabs.x[k] - 30.0f, core::kTabY - 60.0f);
        }
        const float ex = n > 0 ? g_tabs.x[n - 1] + g_tabs.w[n - 1] + 28.0f
                               : core::kLeft + 108.0f;
        Piv("anim-group.tabs.tbE", ex + 30.0f, core::kTabY);
        Piv("anim-group.tabs.tbEHit", ex + 30.0f, core::kTabY);
        if (active >= 0 && active < n) {
            Vis("anim-group.tabs.tbU", true);
            const uint64_t u = engine::widget::Page("anim-group.tabs.tbU");
            if (u) {
                engine::widget::Pivot(u, g_tabs.x[active], core::kTabY + 38.0f);
                engine::widget::Scale(u, g_tabs.w[active] / 100.0f, 1.0f);
            }
        } else Vis("anim-group.tabs.tbU", false);
    }

    int TabHover()
    {
        if (HoverDisabled()) return -1;
        float cx = 0.0f, cy = 0.0f;
        if (!engine::window::CursorCanvas(&cx, &cy)) return -1;
        return core::TabHover(cx, cy, g_tabs);
    }

    void Reset()
    {
        for (Glide& g : g_gl) g = Glide{};
        g_moving = false;
        g_lastTick = 0;
        g_nbox = 0;
        for (core::PromptBox& b : g_boxes) b = core::PromptBox{};
        g_tabs = core::TabLayout{};
    }

} // namespace net::ui::nrui
