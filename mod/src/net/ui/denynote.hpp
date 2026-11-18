#pragma once

// Ported from tools/ext/denynote.c: a picker refusal ("Not enough Loadout Points!",
// "Too many of GUN on team.") shown as a soft panel instead of a plain white line.
//
// On the four picker pages (picker-skill / -primary / -secondary / -purchase) and the
// gear-up page the group anim-group.denyNote* is used; in the match HUD the hud-root
// group t2rDenyNote* is used. Elsewhere Show() falls back to the styled notification
// (net::notify) and returns false so the caller keeps its own path.
//
// Widgets: denynote_widgets.py. Tick() runs once per frame from boot::FrameTick.
//
// NOTE: the ext's callers (ui_deny, gunlimit.c gl_say) are not ported yet, so nothing in
// the mod calls Show() at present -- the API is in place for when they land.

namespace net::denynote {

    // Show a refusal notice. Returns true when it shows (the caller sends no plain line),
    // false when there are no widgets for it.
    bool Show(const char* msg, float secs);

    void Tick();       // ext FeatureDenynote

    // Session teardown / leave the match.
    void Reset();

}
