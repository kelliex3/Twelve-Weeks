#pragma once

// Native owner of tools/ext/t2r-ext.c's `xcam_start` *audio routing* when t2r-ext.dll is
// absent. The render camera itself (icam_filter / the final render-camera detour at RVA
// 0x1477750) is NOT ported: there is no native xcam hook, so this bridge does not drive a
// camera. What it does own is the ext's role-2 dispatch:
//   victim + !upgraded + !(shiv || ctxdown) -> exec-music cue
//   victim + shiv + !ctxdown                 -> shiv drums then the shock sting
//   victim + upgraded                        -> no execution music
// Pure rules live in xcam.hpp; this file supplies the producer and the sinks.
//
// PRODUCER: `net::anim::paired::Get()` (ext `g_ps`), polled by Tick() from boot::FrameTick().
// The native execution / pull-out systems publish the same move the ext read at its own
// xcam_start call: role, move id, uid (a once-per-move edge) and kill seconds. A 2013 move id
// is mapped back through gameplay/melee2013 to its ext name ("shiv-stealth-back",
// "pipe-mod-chop-fin", "shiv-ground-execution-left", ...) and its authored kill frame; a
// contextual down is named "ctxdown:*" so Route() suppresses both cues exactly as the ext
// does. Unknown moves use the ext's own fallback "melee-finisher". Upgraded one-shots are
// recognised from the pipe-mod move kind (the ext's g_gx.upg is not replicated).
//
// The bridge yields while the ext is loaded and resets its edge tracker with the match.

namespace net::audio::xcam_bridge {

    inline constexpr bool kEnabled = true;

    // Install the dispatch sink once at startup (a no-op while the ext is loaded).
    void Init();

    // Poll the paired-move state. Call once per frame after the net tick.
    void Tick();

    // Entry a native xcam / paired-move camera hook can call directly with the ext's
    // arguments: role 1 = attacker, 2 = victim; `name` the move name; `killS` the authored
    // kill seconds; `upgradedOneShot` the ext's (g_gx.upg && g_gx.role == 2).
    void OnCameraStart(int role, const char* name, float killS, bool upgradedOneShot);

    void Reset();

} // namespace net::audio::xcam_bridge
