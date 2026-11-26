#pragma once

#include "nrui-core.hpp"

#include <cstdint>

// Native owner of the shared No Return page frame. The pure layout / width /
// glide rules live in nrui-core.hpp; this module supplies every engine seam
// (gui2 widgets, cursor, pad mode, locale, sidecars) and is shared by the
// CLAN / achievements / customize / map-vote pages.
//
// Coarse nr_tw widths remain for keys, headers, sections, chips and tabs.
// Prompt labels use current main's fine NR_CWF (nr_cwf / nr_twf) and the +26
// tail. Sidecars are read-only; no page stack or tick ownership lives here
// (callers own their pages, boot owns the tick).
//
// Widget writes are inert when the GUI pak does not carry nr_style.py's
// hd*/wk*/pbar*/tabs* groups (engine::widget::Page returns 0).

namespace net::ui::nrui {

    struct Prompt {
        const char* kb = nullptr;    // engine glyph id(s) or plain key text
        const char* pad = nullptr;   // controller glyph, used in pad mode
        const char* label = nullptr;
        bool        on = true;
    };

    // ---- header / week / section / chip ---------------------------------------
    void Header(const char* parent, const char* page);
    void Week(int week, int day, int survived);
    void WeekVisible(bool on);

    // nr_section writes <base>Tx / <base>Rt and places <base>Rl.
    void Section(const char* base, float x0, float x1, float y,
                 const char* title, const char* right, float titlePt);

    // nr_chip sizes <base>Bg / <base>Tx around `text`; right > 0 right-aligns.
    // Returns the laid-out width.
    float Chip(const char* base, const char* text, float x, float right, float pt);

    // ---- boxed prompts ---------------------------------------------------------
    // Writes pb0..pb(n-1) right-to-left from NR_R1 (at most kNPrompt).
    void Prompts(const Prompt* items, int n);
    // Declaration index of the prompt under the mouse, -1 for none / pad mode.
    int  PromptHover();

    bool HoverDisabled();   // pad mode / no cursor (the ext's early return)

    // ---- movable focus frame + scroll thumb ------------------------------------
    void FrameAt(const char* base, float x, float y, float w, float h, bool on);
    void ScrollV(const char* base, float x, float y0, float y1, float h0,
                 int first, int shown, int n);

    // Exponential glides, once per frame after the page writers. `now` is the
    // same GetTickCount64 clock the ext used; t2r-gu-noanim.txt snaps instead.
    void Tick(uint64_t now);

    // ---- clan tab strip + extension pages --------------------------------------
    // A page registers itself once as a tab after the five built-in CLAN tabs.
    // `key` identifies the registration (dedupe), `avail` gates it, `open`
    // pushes the page, `page` returns its page id for top-page routing.
    using AvailFn = bool (*)();
    using OpenFn  = void (*)(const char* why);
    using PageFn  = uint64_t (*)();

    int  AddXTab(const char* label, const void* key, AvailFn avail,
                 OpenFn open, PageFn page);
    int  TabCount();
    const char* TabLabel(int tab);
    int  TabOfPage(uint64_t top);       // index of an extension page on top, -1 none
    bool Route(uint64_t top);           // an extension page is on top
    void Go(int tab, const char* why);  // wraps; calls SetClanGo for 0..4
    void SetClanGo(void (*go)(int tab, const char* why));
    void DrawTabs(int active, int hover);
    int  TabHover();

    void Reset();   // runtime only (glides / cached layouts); keeps registrations

} // namespace net::ui::nrui
