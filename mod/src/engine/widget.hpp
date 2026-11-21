#pragma once

#include <cstdint>
#include <cstddef>

// Ported from the widget helpers in tools/ext/t2r-ext.c (Hudw / UiW / TopPage / PageId /
// WVisible / WAlpha / FeedAlpha / WScale / WPivot / FeedTextW / DnWrapW). They were
// feature-local statics in the ext; here they are one reusable module so every HUD feature
// (notify, denynote, ...) resolves and drives its widgets the same way.
//
// Two kinds of handle:
//   Hud(rel)   a widget under canvas.hudRoot, e.g. "t2rNtL"      (ext Hudw)
//   Page(rel)  a widget under the top of the HUD menu stack        (ext UiW)
// Both return 0 when the widget is not there (older gui2-widgets.pak), which is the
// "feature not available" signal every caller checks.
//
// The draw values go through the engine's own gui2 setters. Like the ext, Visible() and
// Pivot() skip a write that repeats the widget's last value while a rebuilt "R3" page (the
// pickers / gear up) is up -- writing them every frame there made the engine miss draws and
// restarted the page's own focus fades. On every other page the write goes out every frame,
// as the ext's callers rely on (some re-assert against the engine's own animations).

namespace engine::widget {

    uint64_t Hud(const char* rel);            // canvas.hudRoot.<rel>, 0 if absent
    uint64_t Page(const char* rel);           // top-of-hud-stack page's <rel>, 0 if absent
    uint64_t TopPage();                       // the top page of the HUD menu stack
    uint64_t PageId(const char* page);        // "canvas.hudRoot.hudMenuStack.to1/<page>" sid

    void Visible(uint64_t w, bool on);
    void Alpha(uint64_t w, float a);          // the ext's plain FeedAlpha (never deduped)
    void Scale(uint64_t w, float sx, float sy);
    void Pivot(uint64_t w, float x, float y);

    void SetText(uint64_t w, const char* text);
    void SetImage(uint64_t w, const char* path);

    // ---- mouse navigation (ext UiRectSs / UiMouseOver / UiOverViewport) ---------------
    // The ext's "widget escape bounds": the engine's own hover answer is often too
    // tight (a row widget's clickable area misses its label), so when the engine says
    // "not hovered" a screen-space rect derived from the widget's gui2 transform is
    // checked against the cursor (both in the centred 16:9 canvas). A pinned exe
    // function turns a transform into its four corners; a prologue check makes a game
    // update fail closed (WidgetRect returns false) instead of calling a moved RVA.
    bool WidgetRect(uint64_t w, float* x0, float* y0, float* x1, float* y1);

    // ext UiMouseOver: true when the cursor is over `w`. With `arm` the widget is first
    // marked hoverable (gui2-set-widget-hoverable). A t2r-no-mouse-nav.txt sidecar
    // opts out (false). Unknown bounds never block -- they fall back to the engine's
    // answer (which is false here).
    bool MouseOver(uint64_t w, bool arm);

    // ext UiOverViewport: true when the cursor is inside `w`'s rect, OR the rect /
    // cursor is unknown (never block on unknown bounds). Useful as a click gate.
    bool OverViewport(uint64_t w);

    // The ext's per-letter width estimate (FeedTextW): good enough to wrap a sentence
    // without a real text measurement.
    float TextWidth(const char* s, float scale);

    // Greedy word wrap of `s` into `out` (lines joined by '\n'), breaking when the
    // estimated width would pass `wrapWidth`; at most `maxLines` lines, the rest of the
    // sentence stays on the last. The ext's DnWrapW. Returns the line count.
    int Wrap(const char* s, char* out, size_t cap, float wrapWidth, float scale,
             int maxLines = 4);

    // Forget the per-widget dedup cache (level / page change helper).
    void Reset();

}
