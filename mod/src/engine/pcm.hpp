#pragma once

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
// The ext's 3D voice fields (dyn / ent / wp / rnear / rfar and the per-frame gain ramp) are
// NOT ported: no ported feature is 3D yet. Add them with the first 3D caller.
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

    // The handle still plays its source (ext: g_mixv[slot].on && .src == src).
    bool Live(const Voice& v);

    // Fade the voice out over ms (0 = stop now) if it is still live, then clear the handle.
    void Cut(Voice& v, int ms);

    // Forget the handle (ext: a = b = -1).
    void Invalidate(Voice& v);

}
