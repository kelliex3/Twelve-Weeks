// ported from tools/ext/t2r-ext.c's 2013 melee move table (MELEE2013 / MxMove / MxByMove)

#include "melee2013.hpp"

#include <cstdio>

namespace gameplay::melee2013 {

    namespace {

        // ext SHIV_BACK_KILL_S / SHIV_FRONT_KILL_S: the victim half's blood / blade event frame
        // (30 fps) of the 2013 stealth shiv kills, a hair earlier than the clip's own timing.
        constexpr float kShivBackKillS  = 0.310f;
        constexpr float kShivFrontKillS = 0.580f;

        // ext MELEE2013 (verbatim; the carrier / variant headers are not ported, see the hpp).
        const Move kMoves[] = {
            { "fist-execute-enter-front", kFist, 0, 3.200f },
            { "fist-execute-enter-back", kFist, 3, 2.967f },
            { "fist-stomp-execute-left", kFist, 1, 2.267f },
            { "fist-stomp-execute-right", kFist, 2, 2.267f },
            // 2013 names swapped (net-pipe-execute-left plays pipe-execute)
            { "pipe-execute", kPipe, 1, 3.233f },
            { "pipe-execute-left", kPipe, 2, 2.533f },
            { "machete-decapitate", kMachete, 1, 2.033f },
            { "machete-execute-right", kMachete, 2, 2.033f },
            { "fist-execute-neckbreak-fast-left", kNeckFast, 2, 1.633f },
            { "fist-execute-neckbreak-fast-right", kNeckFast, 1, 1.633f },
            { "fist-execute-neckbreak-fastest-left", kNeckFastest, 2, 0.800f },
            { "fist-execute-neckbreak-fastest-right", kNeckFastest, 1, 0.800f },
            // the 2013 shiv kill (DLC5 insta-shiv): a standing enemy stunned (front) / from behind
            { "fist-insta-shiv-stand-front", kShiv, 0, 1.450f },
            { "fist-insta-shiv-stand-back", kShiv, 3, 1.300f },
            // the base game's stealth shiv kills
            { "shiv-stealth-back", kShiv, 3, kShivBackKillS },
            { "shiv-stealth-front", kShiv, 0, kShivFrontKillS },
            // 2013 upgraded pipe-class one-shot (the victim is DOWNED at its end, never killed)
            { "pipe-mod-chop-fin", kPipeMod, 0, 2.92f },
            { "pipe-mod-chop-fin-back", kPipeMod, 3, 3.32f },
            { "pipe-mod-side-swing-fin", kPipeMod, 1, 3.32f },
            // the 2013 GROUND SHIV execution of a downed victim (PS4 Remastered v1.11 clips)
            { "shiv-ground-execution-left", kShivGround, 1, 2.600f },
            { "shiv-ground-execution-right", kShivGround, 2, 2.600f },
            // the 2013 INTERROGATION pairs
            { "fist-interrogate-enter-left", kInterrogate, 1, 5.000f },
            { "fist-interrogate-enter-right", kInterrogate, 2, 5.000f },
            { "shiv-interrogate-enter-back", kIntShiv, 3, 2.550f },
            { "shiv-interrogate-enter-front-dup", kIntShiv, 0, 2.250f },
        };

        constexpr int kCount = static_cast<int>(sizeof kMoves / sizeof kMoves[0]);

    }

    int Count()
    {
        return kCount;
    }

    const Move& At(int i)
    {
        return kMoves[i];
    }

    uint64_t MoveId(int i)
    {
        char b[80];
        std::snprintf(b, sizeof b, "t1mx-%s", kMoves[i].name);
        return SID(b);
    }

    int ByMove(uint64_t move)
    {
        for (int i = 0; i < kCount; i++)
            if (MoveId(i) == move)
                return i;
        return -1;
    }

    bool IsShivMove(uint64_t move)
    {
        const int i = ByMove(move);
        return i >= 0 && (kMoves[i].kind == kShiv || kMoves[i].kind == kIntShiv);
    }

    bool IsKillDeferMove(uint64_t move)
    {
        const int i = ByMove(move);
        return i >= 0 && (kMoves[i].kind == kShiv || kMoves[i].kind == kIntShiv
                          || kMoves[i].kind == kInterrogate);
    }

}
