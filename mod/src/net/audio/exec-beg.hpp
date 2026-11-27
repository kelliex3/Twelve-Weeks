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
// exists. There is no faction / look -> voice mapping, so the private VXG take
// tables cannot select a record: the live paired-melee feed uses the engine's
// named effort events instead (the ext's refusal fallback), and gun / finisher
// record paths stay inert. female effort-SID selection is unavailable, so the
// engine's default gender take is used. The sidecars are read-only and the
// private vox_beg.h tables stay behind TO1_EXEC_BEG_DATA.
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

    void OnPairedStart(const PairedStart& start);
    bool BeginGun(const GunFeed& feed);
    void FinGasp(const FinisherFeed& feed);
    void Tick();
    void Reset();

    bool Active() noexcept;

} // namespace net::audio::exec_beg
