#pragma once

#include <cstdint>

// Ported from tools/ext/notify.c: the styled HUD notification ("toast"). ONE box at a
// time, horizontally centred, drawn from the game's own soft blob texture in three
// pieces (t2rNtL / t2rNtM / t2rNtR) with a kind icon (t2rNtIc) and the text drawn with
// the debug-draw text routine. Every gameplay notification of ours ends here (the old
// ext's msg_hook / msg_tag / notice.c / denynote.c all push through it).
//
// Two shapes:
//   - a toast   (Push)   : in the match HUD, or a server notice on the menu pages
//                          (anim-group.t2rNfL/M/R/Ic/Tx under the top page).
//   - the 2013 centre banner (Banner): one long thin gold-ish bar, no icon.
//
// Widgets: notify_widgets.py (hud-root) and notify_widgets.add_notify_menu (the menu
// pages). Opt-out: t2r-old-notify.txt next to the exe (the old plain paths stay).
//
// Tick() runs once per frame from boot::FrameTick (it drives the menu pages too);
// Draw() runs inside the match-HUD draw block of net::replication::Tick (the raw text).

namespace net::notify {

    // ext NTF_*: the icon follows the CONTEXT of the message (see KindOf).
    enum NtfKind : int {
        NTF_INFO, NTF_HEAL, NTF_REVIVE, NTF_WARN, NTF_SERVER, NTF_LOADOUT,
        NTF_COVERT, NTF_MOVE, NTF_CRAFT, NTF_BLADE, NTF_BOMB, NTF_SUPPLY,
        NTF_N
    };

    const char* IconPath(int kind);        // the kind's icon path (INFO when out of range)
    int         KindOf(const char* t);     // the kind a message's wording implies (ext NtfKindOf)

    // Queue a toast. Returns true when the styled notification takes the message (the
    // caller then does nothing more); false for the old plain path.
    bool Push(int kind, const char* text, float secs);
    bool Banner(const char* text, float secs);

    // Drop a notification by its text: still queued -> removed; showing -> fades out now.
    void Cancel(const char* text);

    // Drop every queued non-server toast (leaving the match).
    void DropNonserver();

    void Tick();       // ext FeatureNotify
    void Draw();       // ext FeatureNotifyDraw (match HUD only)

    // Session teardown / leave the match.
    void Reset();

}
