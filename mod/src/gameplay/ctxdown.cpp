// ported from tools/ext/ctxdown.c (CD_PAIRS + the pure helpers over it)

#include "ctxdown.hpp"

#include "engine/anim-prims.hpp"
#include "sid/sid.hpp"

#include <cstring>

namespace gameplay::ctxdown {

    namespace {

        // ---- the row data ----------------------------------------------------------------
        // The same macro vocabulary ctxdown.c defines before including its generated rows.
        // CD_*_PAIR are data-shaping macros (SCREAMING_SNAKE_CASE per the mod convention).

#define CD_ABBY_PAIR(base, spd, cut) \
    { "abby-fist-" base "--abby", "abby-fist-" base "--npc-f", (spd), (cut) }
#define CD_ELLIE_PAIR(base, spd, cut) \
    { "ellie-knife-" base "--ellie", "ellie-knife-" base "--npc-f", (spd), (cut) }
#define CD_RAW_PAIR(att, vic, spd, cut) { att, vic, (spd), (cut) }
#define CD_WPN_PAIR(base, spd, cut) \
    { "ellie-" base "--ellie", "ellie-" base "--npc-f", (spd), (cut) }
#define CD_WPNA_PAIR(base, spd, cut) \
    { "abby-" base "--abby", "abby-" base "--npc-f", (spd), (cut) }

        // The generated row macros (tools/mv3/gen_duel_*.py), verbatim data.
#include "ctxdown-rows/mduel_moves.h"
#include "ctxdown-rows/mduel_fin.h"
#include "ctxdown-rows/mduel_pins.h"
#include "ctxdown-rows/pullout_rows.h"
#include "ctxdown-rows/mduel_parry.h"

        // ext CD_PAIRS[] -- the hand-authored rows plus the generated row macros, verbatim
        // from ctxdown.c (rows 197-570). Same include technique as the ext.
        const Pair kPairs[] = {
#include "ctxdown-rows/cd-pairs.inc"
        };

        constexpr int kCount = static_cast<int>(sizeof kPairs / sizeof kPairs[0]);

        // ext CD_SLIDE_EXTRA: the victim went to the downed pose too instantly, the slide
        // plays a bit longer.
        constexpr float kSlideExtra = 0.70f;

    }

    int Count()
    {
        return kCount;
    }

    const Pair& At(int i)
    {
        return kPairs[i];
    }

    int BaseLen(const char* n)
    {
        const char* t = std::strchr(n, '~');
        return t ? static_cast<int>(t - n) : static_cast<int>(std::strlen(n));
    }

    bool SameMove(int a, int b)
    {
        const int la = BaseLen(kPairs[a].name);
        return la == BaseLen(kPairs[b].name)
            && !std::strncmp(kPairs[a].name, kPairs[b].name, static_cast<size_t>(la));
    }

    bool IsMove(const char* base, int p)
    {
        const int l = BaseLen(kPairs[p].name);
        return static_cast<int>(std::strlen(base)) == l
            && !std::strncmp(base, kPairs[p].name, static_cast<size_t>(l));
    }

    bool HasPush(int p)
    {
        return kPairs[p].nseg > 1
            && std::strstr(kPairs[p].seg[0].att, "-enter") != nullptr
            && std::strstr(kPairs[p].seg[0].att, "-hit") != nullptr;
    }

    float SegLen(int p, int i)
    {
        const Seg& s = kPairs[p].seg[i];
        const float a = engine::anim::Len(SID("player"), s.att);
        const float v = engine::anim::Len(SID("player"), s.vic);
        const float spd = s.spd > 1.001f ? s.spd : 1.0f;
        float l = a < v ? a : v;
        if (s.cut > 0.0f && s.cut < l)
            l = s.cut;
        return l / spd;
    }

    float FinLen(int p)
    {
        const Pair& r = kPairs[p];
        if (r.kind >= 3)
            return r.end_s;         // dodge counter rows: the hand-off is the row's end
        const int l = r.nseg - 1;
        const float a = engine::anim::Len(SID("player"), r.seg[l].att);
        const float v = engine::anim::Len(SID("player"), r.seg[l].vic);
        const float m = a < v ? a : v;
        float want = r.end_s + kSlideExtra;
        if (m > 0.0f && want > m - 0.05f)
            want = m - 0.05f;
        return want > r.end_s + 0.10f ? want : r.end_s + 0.10f;
    }

    float ChainTotal(int p)
    {
        float t = 0.0f;
        for (int i = 0; i + 1 < kPairs[p].nseg; i++)
            t += SegLen(p, i);
        return t + FinLen(p);
    }

}
