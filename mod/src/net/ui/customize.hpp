#pragma once

#include "customize-core.hpp"

#include <cstdint>

// Native owner of build-191 CUSTOMIZE (tools/ext/customize.c). The profile /
// file / wire rules stay in customize-core.hpp. This owner drives the page,
// the shared in-memory profile, the atomic save and the NEVT_CUSTOM_LOOK send;
// it yields while t2r-ext.dll is loaded (the ext is the deprecated path).
//
// The helpers in `ui` are the small widget-writing seam shared with the
// CUSTOMIZE | CHALLENGES owner. Every one tolerates a missing widget (older
// gui2-widgets.pak): a zero handle is a no-op, never a reason to skip a tick.
namespace net::ui::customize {

    void Tick(uint64_t now);
    void Reset();

    bool Active();                       // this page is on top and owned by mod/
    core::Profile& GetProfile();

    // The challenges page edits the same profile and calls these after a
    // successful equip / tip dismissal.
    void SaveProfile();
    void Changed();                      // profile was edited through GetProfile(): save + bump the wire counter
    void ProgressChanged();              // progress/grants edited through GetProfile(): save + refill, no look wire change
    bool NetChanged();
    uint8_t ConsumeNetChange();          // clears the dirty bit, returns the counter

    int  EditingFaction();
    int  EditingCategory();
    int  Sex(int fac);                   // shown/edited sex of a faction

    // The faction the player currently plays as (-1 while unknown). Mirrors
    // the ext's clan_side_now() gate for the "not your current faction" note.
    int  CurrentSide();

    // Read-only build-191 owner gates shared with the CHALLENGES page: story
    // masks are dev-only (release always hides them) and KELLIE's hid rows are
    // listed only for her own Steam account. Neither writes a sidecar.
    bool StoryMasksShown();
    bool IsKellie();

    bool TipUp();
    void TipDone();                      // latch CZF_CHTIP, save, hide the tip
    void ChallengesLeft();               // re-arm held keys after the pushed page closes

    namespace ui {

        uint64_t W(const char* rel);     // Page(rel), 0 when absent
        void Vis(const char* rel, bool on);
        void Alpha(const char* rel, float a);
        void Text(const char* rel, const char* text);   // locale::Tr
        void RawText(const char* rel, const char* text);
        void Image(const char* rel, const char* path);
        void Pivot(const char* rel, float x, float y);
        void Scale(const char* rel, float sx, float sy);
        void Rule(const char* rel, float x0, float y, float x1, float baseWidth);
        void Bar(const char* rel, float x0, float x1, float y, float frac);
        void Fit(const char* rel, const char* text, float box, float pt);
        float TextWidth(const char* text, float pt);
        void Sfx(const char* name);      // AUDIO::PLAY_SOUND(SID(name))

    } // namespace ui

} // namespace net::ui::customize
