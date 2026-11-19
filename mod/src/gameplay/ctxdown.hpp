#pragma once

#include <cstdint>

// Ported from tools/ext/ctxdown.c: the contextual melee down pair table (CD_PAIRS) and the
// pure helpers over it (CdBaseLen / CdSameMove / CdIsMove / CdHasPush / CdSegLen / CdFinLen /
// CdChainTotal). The melee hit that DOWNS a standing enemy looks at what is right behind the
// victim, the way TLOU2 single-player melee does for a fist "wall" / "low-cover" opener
// finisher: each row is a CHAIN of resident clip PAIRS (attacker half / victim half) played by
// psync on one clock.
//
// The row data is generated (tools/mv3/gen_duel_*.py) plus hand-authored; both live verbatim
// in gameplay/ctxdown-rows/ and are included into the table exactly as ctxdown.c includes
// them. Only the pure helpers are here: the picking / execution logic (the wall probe, the
// loaded check, the contextual swing) belongs to the ctxdown feature and is not part of this
// dependency module.

namespace gameplay::ctxdown {

    inline constexpr int kMaxSeg = 5;   // ext CD_MAXSEG

    // One segment of a chain: the pair of resident clips (attacker half / victim half), the
    // authored speed and the clip seconds played before the switch (0 = the shorter half's
    // whole clip). ext CdSeg.
    struct Seg {
        const char* att;
        const char* vic;
        float       spd;
        float       cut;
    };

    // One contextual-down row. ext CD_PAIRS element.
    struct Pair {
        const char* name;
        int         kind;       // 1 wall, 2 low cover / ledge, 3+ the dodge counters, 7 duel, 8 prone pull-out
        float       dmax;       // wall-dist-max of the move's start lambda
        float       end_s;      // the finisher's victim pelvis on the floor
        int         style;      // -1 unarmed fists, 3 = Ellie's knife set
        int         nseg;
        Seg         seg[kMaxSeg];
    };

    int         Count();
    const Pair& At(int i);

    // "move~variant": the base name of a row (variants differ only in their push). ext CdBaseLen.
    int  BaseLen(const char* n);

    // Do rows a and b share a base name? ext CdSameMove.
    bool SameMove(int a, int b);

    // Is `base` the base name of row p? ext CdIsMove.
    bool IsMove(const char* base, int p);

    // Is row p a COMBO move (its first segment is the push, a "-hit" reaction pair)? ext CdHasPush.
    bool HasPush(int p);

    // Real-time length (s) of segment i: the shorter half / speed, capped by the segment's cut.
    // Reads the clips' durations through engine::anim::Len. ext CdSegLen.
    float SegLen(int p, int i);

    // The finisher's real seconds to the hand-off (pelvis on the floor + CD_SLIDE_EXTRA).
    // ext CdFinLen.
    float FinLen(int p);

    // The whole takedown in real seconds (the segments before the last + the finisher).
    // ext CdChainTotal.
    float ChainTotal(int p);

}
