#pragma once

#include "sid/sid.hpp"

#include <cstdint>

// Ported from tools/ext/t2r-ext.c's 2013 melee move table (MELEE2013 / MxMove / MxByMove /
// MxIsShivMove / MxIsKillDeferMove).
//
// The TLOU2 Factions 1.0 (2013) multiplayer executions, one row per baked clip pair. The
// psync / wire move id of a pair is `fnv64("t1mx-<name>")` -- the id the victim and every
// third party find the clips by. `kind` is the weapon class / quadrant convention (fists by
// quadrant, pipe and machete by half plane, the neck-breaks, the shivs, the interrogations);
// `killS` is the move's kill frame K / 30 (seconds), the time the victim's client kills it.
//
// The ext also carried meleeexec_carriers.h / meleeexec_variants.h (the baked half clip names
// and the weapon-class attacker variants). Those generators are not shipped
// (tools/ext/GENERATED.md) and bloodbuild does not read them, so they are not ported; a
// feature that needs the carrier names adds them when the generator is available.

namespace gameplay::melee2013 {

    // The 2013 weapon-class / quadrant convention (ext MX_*).
    enum Kind {
        kFist,          // dir by quadrant: 0 front, 1 their left, 2 their right, 3 back
        kPipe,          // every blunt weapon (2013 pipe / 2x4 / bat), by half plane
        kMachete,       // blades, by half plane
        kNeckFast,      // Lethal Efficiency I
        kNeckFastest,   // Lethal Efficiency II
        kShiv,          // a standing shiv kill (front / back)
        kShivGround,    // the shiv execution of a LYING victim (PS4 Remastered v1.11 clips)
        kInterrogate,   // the 2013 interrogation of a downed enemy (fists)
        kIntShiv,       // the 2013 shiv interrogation of a standing enemy
        kPipeMod        // the 2013 upgraded (crafted) melee one-shot finisher (downs, never kills)
    };

    struct Move {
        const char* name;
        int         kind;   // Kind
        int         dir;    // quadrant / half plane
        float       killS;  // kill frame K / 30 (seconds)
    };

    int         Count();
    const Move& At(int i);

    // fnv64("t1mx-<name>") -- the psync / wire move id.
    uint64_t MoveId(int i);

    // The row whose MoveId is `move`, or -1 (ext MxByMove).
    int  ByMove(uint64_t move);

    // Is this move id one of the shiv kills (kShiv / kIntShiv)? (ext MxIsShivMove)
    bool IsShivMove(uint64_t move);

    // Moves whose puppet kill is held until the victim half is over (the shivs and the fist
    // interrogation: its kill frame sits well inside a much longer half). ext MxIsKillDeferMove.
    bool IsKillDeferMove(uint64_t move);

}
