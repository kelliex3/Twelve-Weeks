#pragma once

// Native sole owner of tools/ext/exec_beg.c: the execution victim's adrenaline
// plea through the engine's speech path, cut by each shot, with bullet-hit
// grunts after non-final shots and silence (or a headshot death take) on the
// death shot. The same state covers the short melee pleas and the dismember
// scream chain. Every rule lives in exec-beg-core.hpp; this header is only the
// feed API and the query surface.
//
// Who calls it: xcam-bridge.cpp reports the once-per-paired-move edge through
// OnPairedStart(); boot::FrameTick() calls Tick() each frame (before exec
// music so the duck is applied before that owner's tick); net-replication.cpp
// calls Reset() on a level / session reset. BeginGun() is the future producer
// for ext's gun-execution pairs; the native port has no such producer yet, so
// it has no caller. FinGasp() is the ps31 finisher feed, likewise waiting for
// a producer and the voice mapping. No new wire message is added.
//
// Honest gaps: there is no native GUNEXEC_PAIRS / GUNEXEC_AIM / gx_shot_sched()
// or third-party state, so the gun timeline is state-only until that producer
// exists. There is no faction / look -> VXG voice mapping, so the private VXG
// take tables cannot select a record: the live paired-melee feed uses the
// engine's named effort events (the ext's refusal fallback), with female
// speakers overridden by net::audio::effort::EffortSid(); gun / finisher
// record paths stay inert. The sidecars are read-only and the private
// vox_beg.h tables stay behind TO1_EXEC_BEG_DATA.
//
// boot::ExtLoaded() yields every entry point; state resets with the match.

#include "exec-beg-core.hpp"

#include <cstdint>

namespace net::audio::exec_beg {

    // The paired-move fields the owner needs, copied by value so no net/anim
    // header leaks into this API. `vicEnt` is the body the camera shows for the
    // victim (SID("player") on our own screen).
    struct PairedStart {
        uint32_t uid     = 0;
        int      role    = 0;
        int      kind    = 0;
        int      attSlot = -1;
        int      vicSlot = -1;
        uint64_t vicEnt  = 0;
        uint64_t move    = 0;
        float    killS   = 0.0f;
    };

    // The gun-execution feed that is missing natively. `shots` are the filtered
    // (non-arrow) shot times on the ext's paired-move clock; `limb` indexes that
    // array, and `weaponClass` is 1 pistol / 2 rifle / 3 shotgun.
    struct GunFeed {
        uint64_t        ent         = 0;
        int             pair        = -1;
        int             fac         = -1;
        int             female      = -1;
        int             voice       = -1;
        uint32_t        seed        = 0;
        double          moveMs      = 0.0;
        float           skipMs      = 0.0;
        int             weaponClass = 1;
        const uint16_t* shots       = nullptr;
        int             shotCount   = 0;
        int             limb        = -1;
        bool            lastHead    = false;
    };

    // execsp.c's EST_BEG rows. The exec_sp owner arms these instead of the
    // single immediate plea when the pair has authored plea times. `ems` is the
    // execsp clock at the begin call; `earliestMs` / `cutMs` are that row's
    // earliest start and the blow that cuts it; `salt` is ebm_pick's row salt.
    struct MeleePleaFeed {
        double   ems        = 0.0;
        double   earliestMs = 0.0;
        double   cutMs      = 0.0;
        uint32_t salt       = 0;
    };

    // ps31 finisher gasp feed (also inert until a producer and mapping exist).
    struct FinisherFeed {
        uint64_t ent    = 0;
        uint32_t slot   = 0;
        uint32_t seed   = 0;
        int      fac    = -1;
        int      female = -1;
        int      voice  = -1;
        const char* why = nullptr;
    };

    // `armPlea` is false only for pairs whose authored execsp EST_BEG rows will
    // be scheduled by exec_sp instead of the single kill-frame plea.
    void OnPairedStart(const PairedStart& start, bool armPlea = true);
    // Add a scheduled EST_BEG plea to the active melee run. Does nothing when
    // exec_beg has no run (or the private table refused the take).
    bool ScheduleMeleePlea(const MeleePleaFeed& feed);
    bool BeginGun(const GunFeed& feed);
    void FinGasp(const FinisherFeed& feed);
    void Tick();
    void Reset();

    bool Active() noexcept;
    // ext eb_covers_gun / eb_covers: the voice held by a plea (or by execsp's
    // SP victim timeline) so the other audio layers can duck. `ems` is this
    // owner's move clock in ms.
    bool CoversGun(double ems) noexcept;
    bool Covers(double ems) noexcept;

} // namespace net::audio::exec_beg
