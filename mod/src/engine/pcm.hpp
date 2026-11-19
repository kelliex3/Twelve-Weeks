#pragma once

#include "sid/sid.hpp"      // StringId64

#include <cstdint>

// Ported from tools/ext/t2r-ext.c's PCM mixer (the "stereo waveOut" block after
// sfx_user_volume, plus loadseq.c's raw-PCM player). The ext's extra sounds are raw 16-bit
// PCM buffers the engine's own audio system does not play (stings, music stings, footsteps,
// foley), so they go through one waveOut stream of our own: a software mixer thread resamples
// every live voice to 48 kHz stereo into 6 x 20 ms buffers fed by an event callback. This is
// the mechanism ~16 features sit on (PHASE1.md item 4).
//
// Behaviour kept: one stream for the process lifetime (never open/close per sound), a soft
// limiter (knee 0.8, tanh to 1.0) so summed voices never hard-clip, a 2 ms fade in / out per
// voice (no click at a start or a cut), a stereo wave as two hard-panned voices taken in one
// lock (sample-locked), and reopen-after-repeated-write-failure (a lost device). Behaviour
// dropped: the ext's page-fault / prefetch / warm-worker and lock-timing instrumentation
// (dev-only perf work, not behaviour) and the mix-dump dev file.
//
// 3D voices (ext g_mixv's dyn/ent/wp/rnear/rfar and sfx_voices_update): a voice whose gain
// and pan are recomputed every frame from the source body's live position, ramped in the
// mixer (no zipper noise). The listener is the game camera -- see the divergence note on
// ListenerPosDir() in pcm.cpp (the ext's primary was a render-camera hook the mod does not
// port).
//
// The gain model (SfxUserVolume / MusicUserVolume) reads the game's own options sliders
// (volume-sfx / volume-music, squared like the game's taper) times the general volume (mixer
// group 64), so our sounds obey the sliders. 1.0 when the options cannot be read.
//
// Voices are handles, not raw slots: a Voice is valid only while its slot still plays the
// source it was started with (the ext compared g_mixv[v].src), so a slot reused by another
// sound makes Live() false and Cut() a no-op.

namespace engine::pcm {

    using Sample = int16_t;

    // ---- gain model (ext SfxUserVolume / ExmusUserVolume) --------------------

    // The game's SFX slider as our mixer gain (options volume-sfx, squared, x general
    // volume). Re-read at most every 2 s; 1.0 when the options cannot be read.
    float SfxUserVolume();

    // The game's music slider the same way (ext exmus_user_volume).
    float MusicUserVolume();

    // ---- voices --------------------------------------------------------------

    struct Voice {
        int           slot = -1;        // mixer slot, -1 = none
        const Sample* src  = nullptr;   // the source this voice was started with
    };

    // Start the mixer if it is not running. false = no audio device (do not start voices).
    bool Start();

    // One voice, sfx volume applied to gl/gr (ext PcmPlayVoice / PcmPlayLr).
    Voice Play(const Sample* src, int n, float gl, float gr, int rate);

    // A stereo wave as two hard-panned voices, one lock, sfx volume applied to gain
    // (ext PcmPlayStereo).
    bool PlayStereo(const Sample* l, const Sample* r, int n, float gain, int rate,
                    Voice& outL, Voice& outR);

    // A stereo wave as two hard-panned voices at an EXPLICIT gain, no volume applied (the
    // caller already applied MusicUserVolume). The ext's raw stereo start used by the stings.
    bool PlayStereoRaw(const Sample* l, const Sample* r, int n, float gain, int rate,
                       Voice& outL, Voice& outR);

    // ---- 3D voices (ext PcmPlayAt2 / sfx_voices_update) ----------------------

    // One voice placed in the world: gain + pan come from the source body's live position
    // relative to the listener (the camera). `ent` = SID("player") for our own body, a
    // puppet sid otherwise, or 0 when only `wpos` is known. Our own body plays full-level
    // 2D if the spatial query fails (it IS ours); another body / a position is never 2D at
    // full level -- on a failed query its last known position (or wpos) is used, and with
    // neither the sound is dropped. The voice follows the body live (call VoicesUpdate()).
    Voice PlayAt(const Sample* src, int n, float gain, int rate, StringId64 ent,
                 const float* wpos = nullptr);

    // A world-position voice with its own short range: full within rnear m of our body,
    // ((rfar - d) / (rfar - rnear))^2 to silent at rfar, followed live like any 3D voice.
    // The ext's pcm_play_at_range (the placed bomb's sizzle).
    Voice PlayAtRange(const Sample* src, int n, float gain, int rate,
                      const float* wpos, float rnear, float rfar);

    // Recompute every live 3D voice's target gain/pan from the listener and each source's
    // live position. Call once per frame (boot::FrameTick). ext sfx_voices_update.
    void VoicesUpdate();

    // The handle still plays its source (ext: g_mixv[slot].on && .src == src).
    bool Live(const Voice& v);

    // Fade the voice out over ms (0 = stop now) if it is still live, then clear the handle.
    void Cut(Voice& v, int ms);

    // Forget the handle (ext: a = b = -1).
    void Invalidate(Voice& v);

}
