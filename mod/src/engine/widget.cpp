// ported from the widget helpers in tools/ext/t2r-ext.c

#include "widget.hpp"

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp (it uses it in a template)
#include "engine/exe_side.hpp"
#include "engine/frame.hpp"
#include "engine/ptr.hpp"
#include "engine/script.hpp"
#include "engine/window.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace engine::widget {

    namespace {

        // ext FeedTextW constants (clan_widgets.FEED_*)
        constexpr float kFeedPt    = 30.0f;
        constexpr float kFeedGain  = 1.08f;
        constexpr float kFeedTrack = 1.5f;

        constexpr uintptr_t kInvalid = ~static_cast<uintptr_t>(0);

        // ---- Hud handle cache (the ext cached 24 by name) ----------------------
        struct HudEntry {
            char     rel[40];
            uint64_t w;
        };
        HudEntry g_hudCache[24];
        int      g_hudCacheN = 0;

        // ---- Visible()/Pivot() dedup (the ext's UiPropDue) ---------------------
        struct DueEntry {
            uint64_t w;
            uint64_t v;
            uint64_t at;
        };
        constexpr int kDueCap = 64;
        DueEntry g_due[2][kDueCap];     // 0 = visible, 1 = pivot
        uint64_t g_duePage = 0;

        // The ext's LoR3(): is the R3 gear-up / picker page on top this frame? (A write of
        // the same value every frame on those pages made the engine miss draws.) Cached per
        // frame like the ext.
        bool OnR3()
        {
            const uint64_t f = ::engine::frame::Count();
            static uint64_t at = ~static_cast<uint64_t>(0);
            static bool     v  = false;
            if (f != at) {
                at = f;
                v  = Page("anim-group.t3Lines") != 0;
            }
            return v;
        }

        // "Skip a write that repeats the widget's last value" -- 5 s re-assert, cache
        // restarted when the top page changes. Returns true when the write should go out.
        bool Due(int kind, uint64_t w, uint64_t v)
        {
            const uint64_t now = GetTickCount64();
            const uint64_t tp  = TopPage();
            if (tp != g_duePage) {
                g_duePage = tp;
                memset(g_due, 0, sizeof g_due);
            }
            DueEntry* C = g_due[kind];
            for (int i = 0; i < kDueCap; i++) {
                if (C[i].w == w) {
                    if (C[i].v == v && now - C[i].at < 5000)
                        return false;
                    C[i].v  = v;
                    C[i].at = now;
                    return true;
                }
                if (!C[i].w) {
                    C[i].w  = w;
                    C[i].v  = v;
                    C[i].at = now;
                    return true;
                }
            }
            int oldest = 0;                      // full: overwrite the stalest
            for (int i = 1; i < kDueCap; i++)
                if (C[i].at < C[oldest].at)
                    oldest = i;
            C[oldest].w  = w;
            C[oldest].v  = v;
            C[oldest].at = now;
            return true;
        }

    }

    uint64_t Hud(const char* rel)
    {
        if (!rel)
            return 0;
        const size_t rl = strlen(rel);
        if (rl < sizeof g_hudCache[0].rel)
            for (int i = 0; i < g_hudCacheN; i++)
                if (!memcmp(g_hudCache[i].rel, rel, rl + 1))
                    return g_hudCache[i].w;

        // The handle is the hash of the full path, so a found one stays valid: cache it.
        const uint64_t w = DC::Call<"gui2-build-widget-path">(
            SID("canvas.hudRoot"), rel, 0, 0, 0, 0, 0, 0, 0);
        if (w == kInvalid)
            return 0;
        if (w && rl < sizeof g_hudCache[0].rel && g_hudCacheN < 24) {
            memcpy(g_hudCache[g_hudCacheN].rel, rel, rl + 1);
            g_hudCache[g_hudCacheN].w = w;
            g_hudCacheN++;
        }
        return w;
    }

    uint64_t Page(const char* rel)
    {
        if (!rel)
            return 0;
        return MENU2::GET_PATH_IN_CURRENT_PAGE(rel, MenuStack::hudMenuStack);
    }

    uint64_t TopPage()
    {
        return MENU2::GET_TOP_PAGE_IN_STACK(MenuStack::hudMenuStack);
    }

    uint64_t PageOnStack(const char* rel, MenuStack stack)
    {
        if (!rel) return 0;
        return MENU2::GET_PATH_IN_CURRENT_PAGE(rel, stack);
    }

    uint64_t TopPage(MenuStack stack)
    {
        return MENU2::GET_TOP_PAGE_IN_STACK(stack);
    }

    uint64_t PageId(const char* page)
    {
        char b[96];
        snprintf(b, sizeof b, "canvas.hudRoot.hudMenuStack.to1/%s", page ? page : "");
        return SID(b);
    }

    void Visible(uint64_t w, bool on)
    {
        if (!w)
            return;
        if (OnR3() && !Due(0, w, on ? 1u : 0u))
            return;
        DC::Call<"gui2-set-widget-visible">(w, on);
    }

    void Alpha(uint64_t w, float a)
    {
        if (!w)
            return;
        DC::Call<"gui2-set-widget-alpha">(w, a);
    }

    void Scale(uint64_t w, float sx, float sy)
    {
        if (!w)
            return;
        const uint64_t xf = DC::Call<"gui2-get-widget-transform">(w);
        if (!xf || xf == kInvalid || !::engine::ptr::Readable(xf, 0x40))
            return;
        float v[4] = { sx, sy, 0.0f, 0.0f };
        DC::Call<"gui2-rxfm-set-scale-ps">(xf, reinterpret_cast<uint64_t>(v));
    }

    void Pivot(uint64_t w, float x, float y)
    {
        if (!w)
            return;
        union { float f; uint32_t u; } qx = { x }, qy = { y };
        const uint64_t key = (static_cast<uint64_t>(qx.u) << 32) | qy.u;
        if (OnR3() && !Due(1, w, key))
            return;
        const uint64_t xf = DC::Call<"gui2-get-widget-transform">(w);
        if (!xf || xf == kInvalid || !::engine::ptr::Readable(xf, 0x28))
            return;
        float v[4] = { x, y, 0.0f, 0.0f };
        DC::Call<"gui2-rxfm-set-pivot-as">(xf, reinterpret_cast<uint64_t>(v));
    }

    bool PivotGet(uint64_t w, float* x, float* y)
    {
        if (!w)
            return false;
        const uint64_t xf = DC::Call<"gui2-get-widget-transform">(w);
        if (!xf || xf == kInvalid || !::engine::ptr::Readable(xf, 0x28))
            return false;
        const float* p = reinterpret_cast<const float*>(xf + 0x20);
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]))
            return false;
        if (x) *x = p[0];
        if (y) *y = p[1];
        return true;
    }

    bool ScaleGet(uint64_t w, float* sx, float* sy)
    {
        if (!w)
            return false;
        const uint64_t xf = DC::Call<"gui2-get-widget-transform">(w);
        if (!xf || xf == kInvalid || !::engine::ptr::Readable(xf, 0x40))
            return false;
        const float* p = reinterpret_cast<const float*>(xf + 0x30);
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]))
            return false;
        if (p[0] <= 0.05f || p[0] >= 20.0f || p[1] <= 0.05f || p[1] >= 20.0f)
            return false;
        if (sx) *sx = p[0];
        if (sy) *sy = p[1];
        return true;
    }

    namespace {
        TextWriteObserver g_textObserver = nullptr;
        void*             g_textObserverUser = nullptr;
    }

    void SetTextObserver(TextWriteObserver fn, void* user)
    {
        g_textObserver = fn;
        g_textObserverUser = user;
    }

    void SetText(uint64_t w, const char* text)
    {
        if (!w)
            return;
        if (g_textObserver)
            g_textObserver(w, text ? text : "", g_textObserverUser);
        GUI2::SET_WIDGET_TEXT(w, text ? text : "");
    }

    void SetImage(uint64_t w, const char* path)
    {
        if (!w || !path)
            return;
        GUI2::SET_WIDGET_IMAGE(w, path);
    }

    // ---- mouse navigation (ext UiRectSs / UiMouseOver / UiOverViewport) ---------------

    bool WidgetRect(uint64_t w, float* x0, float* y0, float* x1, float* y1)
    {
        if (!w || !x0 || !y0 || !x1 || !y1)
            return false;
        // ext UiRectSs: pinned exe function that fills a transform's four corners.
        // The prologue check makes a game update fail closed instead of calling a
        // moved RVA (the ext pins tlou-ii.exe 1.6.10721).
        static const uint8_t kPrologue[7] =
            { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48 };
        constexpr uintptr_t kRva = 0x20029c0;
        uint8_t* exe = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
        if (!exe || std::memcmp(exe + kRva, kPrologue, sizeof kPrologue) != 0)
            return false;

        const uint64_t xf = DC::Call<"gui2-get-widget-transform">(w);
        if (!xf || xf == kInvalid || !::engine::ptr::Readable(xf, 0x60))
            return false;

        using FnCorners = void(__fastcall*)(void*, float*);
        const auto corners = reinterpret_cast<FnCorners>(exe + kRva);
        alignas(32) float c[16];
        corners(reinterpret_cast<void*>(xf), c);

        *x0 = *x1 = c[0];
        *y0 = *y1 = c[1];
        for (int k = 1; k < 4; k++) {
            const float x = c[k * 4], y = c[k * 4 + 1];
            if (!std::isfinite(x) || !std::isfinite(y))
                return false;
            if (x < *x0) *x0 = x;
            if (x > *x1) *x1 = x;
            if (y < *y0) *y0 = y;
            if (y > *y1) *y1 = y;
        }
        return *x1 > *x0 && *y1 > *y0;
    }

    bool MouseOver(uint64_t w, bool arm)
    {
        static int off = -1;
        if (off < 0)
            off = ::engine::exe_side::Exists("t2r-no-mouse-nav.txt") ? 1 : 0;
        if (off || !w)
            return false;

        if (arm)
            DC::Call<"gui2-set-widget-hoverable">(w, 1);
        const uint64_t hov = DC::Call<"gui2-widget-is-currently-mouse-hovered?">(w);
        if ((hov & 0xff) == 1)
            return true;

        // ext "escape bounds": the engine's hover answer is often too tight for a
        // row widget, so a hit inside the transform's screen-space rect also counts.
        float cx = 0.0f, cy = 0.0f, x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        if (::engine::window::CursorCanvas(&cx, &cy)
            && WidgetRect(w, &x0, &y0, &x1, &y1))
            return cx >= x0 && cx <= x1 && cy >= y0 && cy <= y1;
        return false;                 // unknown bounds never ADD a hit
    }

    bool OverViewport(uint64_t w)
    {
        // ext UiOverViewport: unknown rect / cursor never blocks (return true).
        float cx = 0.0f, cy = 0.0f, x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        if (!::engine::window::CursorCanvas(&cx, &cy)
            || !WidgetRect(w, &x0, &y0, &x1, &y1))
            return true;
        return cx >= x0 && cx <= x1 && cy >= y0 && cy <= y1;
    }

    float TextWidth(const char* s, float scale)
    {
        if (!s)
            return 0.0f;
        float w = 0.0f;
        int tag = 0;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; p++) {
            const unsigned char c = *p;
            if (c == '<') { tag = 1; continue; }
            if (tag) { if (c == '>') tag = 0; continue; }
            if ((c & 0xC0) == 0x80) continue;          // UTF-8 continuation
            float em;
            if (c == ' ')                         em = 0.34f;
            else if (strchr(".,:;'!|`", c))       em = 0.30f;
            else if (strchr("ilIjtfr()-/", c))    em = 0.40f;
            else if (c == 'm' || c == 'w')        em = 0.92f;
            else if (c == 'M' || c == 'W')        em = 1.00f;
            else if (c >= 'A' && c <= 'Z')        em = 0.80f;
            else if (c >= '0' && c <= '9')        em = 0.72f;
            else if (c >= 0x80)                   em = 0.66f;
            else                                  em = 0.64f;
            w += em * kFeedPt * kFeedGain * scale + kFeedTrack;
        }
        return w;
    }

    int Wrap(const char* s, char* out, size_t cap, float wrapWidth, float scale, int maxLines)
    {
        if (!s || !out || cap == 0)
            return 0;
        if (maxLines < 1)
            maxLines = 1;

        size_t o = 0;
        int    lines = 1;
        float  lw = 0.0f;
        const char* p = s;
        while (*p && o + 8 < cap) {
            const char* e = p;
            while (*e && *e != ' ' && *e != '\n')
                e++;
            char word[100];
            size_t wl = static_cast<size_t>(e - p);
            if (wl > sizeof word - 1)
                wl = sizeof word - 1;
            memcpy(word, p, wl);
            word[wl] = 0;

            const float ww = TextWidth(word, scale);
            const float sp = TextWidth(" ", scale);
            if (lw > 0.0f && lw + sp + ww > wrapWidth && lines < maxLines) {
                out[o++] = '\n';
                lines++;
                lw = 0.0f;
            }
            else if (lw > 0.0f) {
                out[o++] = ' ';
                lw += sp;
            }
            for (size_t i = 0; i < wl && o + 2 < cap; i++)
                out[o++] = word[i];
            lw += ww;

            p = e;
            if (*p == '\n') {
                if (lines < maxLines) {
                    out[o++] = '\n';
                    lines++;
                    lw = 0.0f;
                }
                p++;
            }
            else {
                while (*p == ' ')
                    p++;
            }
        }
        out[o] = 0;
        return lines;
    }

    void Reset()
    {
        memset(g_due, 0, sizeof g_due);
        g_duePage = 0;
    }

}
