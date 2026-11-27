#pragma once

// Native sole owner of tools/ext/execsp.c: the authored SP layers that fill the
// silent gaps of the 2013 execution timeline (attacker efforts / swing foley,
// victim takes, short melee pleas) and the unarmed contact events. All row,
// timing, pick-salt and contact rules live in exec-sp-core.hpp; this header is
// only the feed API and the query surface.
//
// Who calls it: xcam-bridge.cpp fans the once-per-paired-move edge out to
// OnPairedStart() (after exec_beg has opened the victim-voice run, so the
// scheduled EST_BEG pleas have somewhere to go); boot::FrameTick() calls Tick();
// net-replication.cpp calls Reset(). Contact() is the feed for the absent
// ex_play_label producer, and BeginGun() is the feed for the absent gun-exec
// pair / shot-schedule producer. No new wire message is added.
//
// Honest gaps: the native paired move currently publishes one action SID
// ("fist-throat-punch-kill"), not a 2013 MELEE2013 name, so the live xcam edge
// has no authored rows to arm. A future producer that supplies the 2013 name
// reaches the full timeline through this API. Female speakers override the
// generic say keys with net::audio::effort::EffortSid() (ES_VOX[].vxe); DC
// acceptance is not proof the sound bank is loaded. The sidecars are read-only.
//
// boot::ExtLoaded() yields every entry point.

#include "exec-sp-core.hpp"

#include <cstdint>

namespace net::audio::exec_sp {

    // The paired-move feed. `name` is the 2013 pair name from the producer's
    // table (a static string); `t0` is its zero on the shared move clock and
    // `moveMs` is that clock at this call. `bluntOk` is the producer's weapon
    // class answer for the pipe-class pairs.
    struct MeleeFeed {
        uint32_t    uid     = 0;
        int         pair    = -1;
        const char* name    = nullptr;
        uint64_t    attEnt  = 0;
        uint64_t    vicEnt  = 0;
        double      t0      = 0.0;
        double      moveMs  = 0.0;
        bool        bluntOk = true;
    };

    // The missing gun-execution feed: same clock convention, kind 0. The gun
    // rows only fill gaps between the shot / plea voice, so no shot table is
    // needed here.
    struct GunFeed {
        uint32_t    uid    = 0;
        int         pair   = -1;
        const char* name   = nullptr;
        uint64_t    attEnt = 0;
        uint64_t    vicEnt = 0;
        double      t0     = 0.0;
    };

    // Open a timeline and arm its scheduled EST_BEG pleas through exec_beg.
    // Does nothing when this pair has no authored rows (or the ext owns it).
    void OnPairedStart(const MeleeFeed& feed);
    bool BeginGun(const GunFeed& feed);

    // The absent ex_play_label(exec=1) producer feeds this with the EXS_* label;
    // the pair, clock and victim body come from the active timeline.
    void Contact(core::ContactLabel label);

    // ext iw_copies_shift: a slowed interrogation hold shifts this clock too.
    void ShiftClock(double fromT0, double deltaMs);

    void Tick();
    void Reset();

    bool Active() noexcept;
    // Authored-row query used by xcam to suppress exec_beg's single kill-frame
    // plea when this pair has scheduled EST_BEG rows.
    bool HasPleas(const char* name) noexcept;
    // ext es_holds_victim's live half (sidecar + own_vic + 4.5 s window).
    bool HoldsVictimVoice() noexcept;

} // namespace net::audio::exec_sp
