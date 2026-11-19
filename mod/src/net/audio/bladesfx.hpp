#pragma once

#include "sid/sid.hpp"

#include <cstdint>

// Ported from tools/ext/blade_sfx.h -- TLOU2 blade melee sound design (Ellie's knife /
// machete) as layered raw-PCM cues the engine's own audio system does not play, so they go
// through engine::pcm's mixer, 3D on the body (PlayAt). NOT the 2013 shiv kills, which keep
// their audio.
//
//   - executions: LabelCue() maps the machete / tomahawk / 1h-blade execution labels to the
//     M_* cues. The shiv labels are never mapped.
//   - contextual takedowns: the knife (style 3) and 1h-blade (style 1) pairs use the
//     BLADE_CD table (a cue per pair: swish / hit / heavy / extract); CdAct() drives it.
//
// The wave data (t2blade.pcm / t2blade.h, tools/rip_t2_blade.py) and the EXS_* blade labels
// (t2melee.h) are private generator outputs. Without T2R_DATA_T2BLADE the module compiles and
// every entry point is inert (CuePlay returns 0, LabelCue returns -1).
//
// Opt-outs next to the exe: t2r-no-blade-pcm.txt = off; t2r-melee-sfx-gain.txt (percent) is
// the master gain.

// The cue ids. With the generated header the enum comes from it; otherwise this in-mod mirror
// is used (inert without data). BC_N is the COUNT and doubles as "no cue" (the ext's
// `post < BC_N` test), so it is the last enumerator; names/order must match
// tools/rip_t2_blade.py's t2blade.h.
#ifdef T2R_DATA_T2BLADE
#include "t2melee.h"        // private (rip_t2_melee.py): the EXS_* blade labels
#include "t2blade.h"        // private (rip_t2_blade.py): BC_* cue ids + T2B_* tables + t2b_blob
#else
enum Cue : int {
    BC_K_SWISH = 0,
    BC_K_SLASH,
    BC_K_STAB,
    BC_K_STAB_DEEP,
    BC_K_GUT_SLICE,
    BC_K_NECK_STAB,
    BC_K_NECK_SLICE,
    BC_K_EYE,
    BC_K_HEAD_STAB,
    BC_K_EXTRACT,
    BC_M_WHOOSH,
    BC_M_SLASH,
    BC_M_GUT,
    BC_M_TORSO,
    BC_M_NECK,
    BC_M_EXTRACT,
    BC_N,                   // count / "none"
};
#endif

namespace net::audio::bladesfx {

    // Blade PCM on? (t2r-no-blade-pcm.txt next to the exe = off). ext BladePcmOn.
    bool PcmOn();

    // Master gain (t2r-melee-sfx-gain.txt, percent; default 0.45). ext BladeMaster.
    float Master();

    // One cue (a random variant) at a body `ent` (or a world position `wpos` when there is no
    // body). Returns 1 when a voice was started. ext BladeCuePlay.
    int CuePlay(int cue, float gain, uint64_t ent, const float* wpos, const char* tag);

    // Execution label -> cue, or -1 when the label is not a blade one (the caller keeps its
    // old path; the shiv labels are never mapped). Needs the private EXS_* labels, so -1
    // without T2R_DATA_T2BLADE. ext BladeLabelCue.
    int LabelCue(int label);

    // Contextual-takedown act: 0 = the swish before a blow, 1 = the first blow, 2 = the heavy
    // / final blow. style 3 = knife, 1 = 1h blade (a listed pair of another style plays too:
    // the upgraded crowbar's taped blades). Returns 1 when it played (the caller skips its own
    // label). ext BladeCdAct.
    int CdAct(const char* pair, int style, int act, uint64_t vic, uint64_t att);

    // Fire the deferred "the blade comes back out" cues (BLADE_POST_MS after the heavy blow).
    // Call once per frame (boot::FrameTick). ext BladePendingTick.
    void PendingTick();

}
