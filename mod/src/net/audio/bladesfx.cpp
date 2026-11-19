// ported from tools/ext/blade_sfx.h

#include "bladesfx.hpp"

#include "engine/exe_side.hpp"
#include "engine/flag.hpp"
#include "engine/pcm.hpp"

#include "log.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef T2R_DATA_T2BLADE
#include "t2blade.h"        // generated (private): BC_* cue ids, T2B_CUE / T2B_WAVE / T2B_NAME / T2B_RATE, t2b_blob
#endif

namespace net::audio::bladesfx {

    namespace {

        constexpr uint64_t kFileCacheMs = 2000;    // ext re-reads the sidecars at most every 2 s
        constexpr int      kLogLimit    = 160;     // ext nl++ < 160

        uint64_t g_logN = 0;

        // ext BladePcmOn: t2r-no-blade-pcm.txt next to the exe = off.
        bool PcmOnCached()
        {
            return !engine::flag::Present("t2r-no-blade-pcm.txt");
        }

        // ext BladeMaster: t2r-melee-sfx-gain.txt (percent, 0..400; default 0.45). A value,
        // not a flag, so it needs the file itself; re-read at most every 2 s.
        float MasterCached()
        {
            static float    v = 0.45f;
            static uint64_t last = 0;
            const uint64_t now = GetTickCount64();
            if (now - last <= kFileCacheMs)
                return v;
            last = now;
            v = 0.45f;
            if (FILE* f = engine::exe_side::Open("t2r-melee-sfx-gain.txt", "r")) {
                int pc = 0;
                if (std::fscanf(f, "%d", &pc) == 1 && pc >= 0 && pc <= 400)
                    v = pc / 100.0f;
                std::fclose(f);
            }
            return v;
        }

    }

    bool PcmOn()
    {
        return PcmOnCached();
    }

    float Master()
    {
        return MasterCached();
    }

    int CuePlay(int cue, float gain, uint64_t ent, const float* wpos, const char* tag)
    {
#ifndef T2R_DATA_T2BLADE
        (void)cue; (void)gain; (void)ent; (void)wpos;
        if (g_logN++ < kLogLimit)
            TO1_LOG("[BLADESFX] %s - data not built (T2R_DATA_T2BLADE off)\n", tag ? tag : "?");
        return 0;
#else
        static unsigned rot = 0;
        if (cue < 0 || cue >= BC_N || !T2B_CUE[cue].count)
            return 0;
        const unsigned v =
            T2B_CUE[cue].first + (rot++ * 5u + static_cast<unsigned>(GetTickCount64() >> 4))
                % T2B_CUE[cue].count;
        const engine::pcm::Voice voice =
            engine::pcm::PlayAt(t2b_blob + T2B_WAVE[v].off, static_cast<int>(T2B_WAVE[v].n),
                gain * MasterCached() * T2B_CUE[cue].gain / 100.0f, T2B_RATE, ent, wpos);
        const int ok = voice.slot >= 0 ? 1 : 0;
        if (g_logN++ < kLogLimit)
            TO1_LOG("[BLADESFX] %s: cue %s variant %u -> %d\n", tag ? tag : "?",
                T2B_NAME[cue], v - T2B_CUE[cue].first, ok);
        return ok;
#endif
    }

    int LabelCue(int label)
    {
#ifndef T2R_DATA_T2BLADE
        (void)label;        // the EXS_* blade labels come from the private t2melee.h
        return -1;
#else
        switch (label) {
        case EXS_MACHETE_WHOOSH:                 return BC_M_WHOOSH;
        case EXS_HIT_MACHETE_TORSO_FINISHER:     return BC_M_TORSO;
        case EXS_HIT_MACHETE_TORSO_EXTRACT:      return BC_M_EXTRACT;
        case EXS_HIT_MACHETE_HEAD_FINISHER:      return BC_M_NECK;
        }
        return -1;
#endif
    }

    namespace {

        // {pair name, hit cue, heavy cue, cue after the heavy blow (BC_N = none)}; the swish is
        // the style's. ext BLADE_CD.
        struct BladeCd {
            const char* pair;
            int         hit, heavy, post;
        };
        constexpr BladeCd kBladeCd[] = {
            // ---- Ellie's knife (style 3)
            { "knife-wall-kneed-gut-slice", BC_K_SLASH,    BC_K_GUT_SLICE,  BC_N },
            { "knife-wall-neck-jab",        BC_K_NECK_STAB, BC_K_NECK_STAB,  BC_K_EXTRACT },
            { "knife-wall-neck-slice",      BC_K_SLASH,    BC_K_NECK_SLICE, BC_N },
            { "knife-wall-chest-stab",      BC_K_STAB,     BC_K_STAB_DEEP,  BC_K_EXTRACT },
            { "knife-cover-chest-stab",     BC_K_STAB,     BC_K_STAB_DEEP,  BC_K_EXTRACT },
            { "knife-cover-gut-to-eye",     BC_K_STAB,     BC_K_EYE,        BC_K_EXTRACT },
            { "knife-cover-ice-pick",       BC_K_STAB,     BC_K_HEAD_STAB,  BC_K_EXTRACT },
            { "knife-cover-neck-stab",      BC_K_STAB,     BC_K_NECK_STAB,  BC_K_EXTRACT },
            // ---- 1h blade / machete (style 1)
            { "blade-wall-gut-chop",        BC_M_SLASH,    BC_M_GUT,        BC_M_EXTRACT },
            { "blade-wall-neck-chop",       BC_M_SLASH,    BC_M_NECK,       BC_N },
            { "blade-wall-shoulder-chop",   BC_M_SLASH,    BC_M_TORSO,      BC_M_EXTRACT },
            { "blade-wall-skull-chop",      BC_M_TORSO,    BC_M_NECK,       BC_N },
            { "blade-wall-gut-stab",        BC_M_TORSO,    BC_M_TORSO,      BC_M_EXTRACT },
            { "blade-wall-neck-slice",      BC_M_SLASH,    BC_M_NECK,       BC_N },
            { "blade-cover-hack",           BC_M_SLASH,    BC_M_GUT,        BC_N },
            // ---- the upgraded 1h melee one-shots (ctxdown.c kind 0 rows): the crowbar row is
            //      style 0 (1h blunt) but its taped blades stab the EYE (the knife's layers)
            { "upg-crowbar-eye-jab",        BC_K_EYE,      BC_K_EXTRACT,    BC_N },
            { "upg-blade-gut-chop-kick",    BC_M_GUT,      BC_M_EXTRACT,    BC_N },
        };

        constexpr uint64_t kPostMs = 280;   // ext BLADE_POST_MS: the blade comes back out this long after the heavy blow

        struct Pend {
            double   due = 0.0;
            int      cue = 0;
            uint64_t ent = 0;
            char     tag[40] = {};
        };
        Pend g_pend[4];

    }

    void PendingTick()
    {
        const double now = static_cast<double>(GetTickCount64());
        for (Pend& p : g_pend) {
            if (p.cue >= 0 && p.due && now >= p.due) {
                if (now - p.due < 600.0)
                    CuePlay(p.cue, 1.0f, p.ent, nullptr, p.tag);
                p.due = 0;
            }
        }
    }

    int CdAct(const char* pair, int style, int act, uint64_t vic, uint64_t att)
    {
        bool listed = false;
        for (const BladeCd& r : kBladeCd)
            listed = listed || (pair && std::strcmp(r.pair, pair) == 0);
        if ((style != 1 && style != 3 && style != 4 && !listed) || !PcmOnCached())
            return 0;       // (a listed pair of another style: the upgraded crowbar's taped blades)

        int hit   = style == 3 ? BC_K_STAB : BC_M_SLASH;
        int heavy = style == 3 ? BC_K_STAB_DEEP : BC_M_TORSO;
        int post  = BC_N;
        for (const BladeCd& r : kBladeCd)
            if (pair && std::strcmp(r.pair, pair) == 0) {
                hit   = r.hit;
                heavy = r.heavy;
                post  = r.post;
            }

        if (act == 0)
            return CuePlay(style == 3 ? BC_K_SWISH : BC_M_WHOOSH, 1.0f, att, nullptr, pair);
        if (act == 1)
            return CuePlay(hit, 1.0f, vic, nullptr, pair);

        const int ok = CuePlay(heavy, 1.0f, vic, nullptr, pair);
        if (post < BC_N) {
            for (Pend& p : g_pend)
                if (!p.due) {
                    p.due = static_cast<double>(GetTickCount64()) + kPostMs;
                    p.cue = post;
                    p.ent = vic;
                    std::snprintf(p.tag, sizeof p.tag, "%s", pair ? pair : "");
                    break;
                }
        }
        return ok;
    }

}
