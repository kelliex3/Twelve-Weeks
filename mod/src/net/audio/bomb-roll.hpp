#pragma once

// Native sole owner of the thrown-bomb ground sounds ported from tools/ext/bomb_roll.c
// (the live br_feed / br_impact / br_roll bodies): a floor or side impact plays one of
// TLOU2's own wall / throwable waves at the bomb's point, then a quiet grain every
// 0.30 m while it rolls on the ground. The ext's SP contact events never fire for
// replicated ProjectileThrowable copies, so this rebuilds them from sampled positions.
//
// Who feeds it:
//  - Install() puts one inline safetyhook on DC::GetFunctionAddress<"throw-projectile">().
//    The trampoline's returned SID is the new projectile: its kind comes from the thrown
//    item SID in argv[2], and Watch() registers the handle for per-frame sampling.
//    If the address is missing / already hooked, this fails once (logged) and the
//    external net paths can still call Watch() themselves. No protocol change is made.
//  - Feed(handle, kind, slot, worldPos) is the direct position source (our own throw
//    and the remote projectile loop). A handle already on the watch list is left to
//    Tick() so one bomb is never clocked twice. SetNextFeedSlot(slot) supplies the net
//    slot for a Feed() that did not carry one; it is consumed by the next Feed/hook.
//  - Steer(handle) marks the two frames after a trajectory-stream velocity set;
//    Tick() emits queued cues through the guarded PCM sink; Reset() clears everything.
//
// boot::ExtLoaded() yields every public entry while the legacy ext is loaded. All
// sidecars next to the exe are read-only: t2r-no-bomb-roll.txt (opt-out),
// t2r-bomb-roll-native.txt (opt-in SP event probe, A/B) and t2r-bomb-roll-vol.txt
// (master gain 0..2, 2 s cadence). The flags use a 120-frame cache. When the native
// probe is on, a kind 1 / 2 impact first tries DC "play-sound-at-joint" with
// sfx-pipe-bomb-imp / sfx-stunbomb-bounce and skips our wave if the engine accepted
// it; molotov has no probe.
//
// Data: bomb_roll.pcm is private game-derived audio. Every read of it lives behind
// TO1_BOMB_ROLL_PCM (bomb-roll-pcm.hpp); with the macro off Play() starts no voice
// and no RNG of its own. The pure rules are in bomb-roll-core.hpp and are covered by
// mod/tests/bomb_roll_core_test.cpp. Logs are tagged "[BOMBROLL]".

#include "bomb-roll-core.hpp"

#include <cstdint>

namespace net::audio::bomb_roll {

    // One-time hook install; idempotent, and a no-op while the ext owns the feature.
    void Install();

    // Set the net slot consumed by the next Feed() / hook handle. -1 means our throw.
    void SetNextFeedSlot(int slot);

    // Register / refresh a projectile for Tick()'s world-position sampler.
    void Watch(uint64_t handle, int kind, int slot);
    void Unwatch(uint64_t handle);

    // Direct position feed (dt comes from QPC; cooldowns from GetTickCount64).
    void Feed(uint64_t h, int kind, int slot, const float pos[3]);
    void Steer(uint64_t h);
    void Tick();
    void Reset();

    // TLOU2 throwable SID -> core::Kind (0 = not one of ours).
    int  KindFromSid(uint64_t sid) noexcept;
    bool Active() noexcept;

} // namespace net::audio::bomb_roll
