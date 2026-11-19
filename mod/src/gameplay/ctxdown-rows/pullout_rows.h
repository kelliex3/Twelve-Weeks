// pullout_rows.h - the PULL-OUT pairs (melee_pullout.c, kind 8, style -1: the attacker's hands are emptied like a fist takedown). SP's NPC-pulls-the-player-out-of-prone-cover sequence
// (anim-melee-ellie-npc-t2 / anim-melee-abby-npc-t2, in the MP level adds via PARRY_SP_COMMON_PAKS): npc half = the attacker, player half = the pulled victim (male = abby, female = ellie).
// dmax = the blow moment (success: the victim's counter lands on the attacker; fail: the lethal blow on the victim), end_s = the hand-off (the drag's is also where the counter window ends).
// pu-drag-l/r/fw-<sex> = the other three directions of SP's drag (the NPC on the prone player's left / right / at its head; same paks, no authored camera = the procedural pull-out
// camera); hand-off = clip length - 0.13 s like the bw row. The success / fail pairs are shared by all four directions.
// GRAPPLE (pull out, grapple, then a window to hit F, and a death punch on failure): pu-grap-<sex> = SP's npc-prone-struggle
// (the 4.73 s struggle clips of the same paks; SP ep 8 escaped at 3.34 s = phase 0.705, ep 5 failed at the clip's end). Hand-off 3.40 s: the counter
// window opens 600 ms before it (2.80 s) for 400 ms + 30 ms grace, the outcome (pu-ok / pu-fail) follows at the hand-off.
// Appended at the END of CD_PAIRS (generated tables index the rows before it).
// ps45 prone (variants A / B, melee_pullout.c): pu-kill-l / -r = SP's npc-vs-player-prone-kill-left / -right (anim-melee-npc-t2, actor97 via PARRY_SP_COMMON_PAKS; the NPC
// grabs the prone player and rolls it onto its back by ~0.4 s; npc-f = the attacker, ellie = the victim - no abby half exists). Hand-off 0.45 s [TUNING ps45] (the research
// measured its victim pose within 0.12 m of the struggle's frame 0 at 0.4-0.6 s); the grapple pair follows, solved on the victim's pose there.
#pragma once
#define PUL_CD_ROWS \
    {"pu-drag-abby", 8, 0.0f, 3.20f, -1, 1, {CD_RAW_PAIR("abby-pullout-prone-bw--npc", "abby-pullout-prone-bw--abby", 1.0f, 0.0f)}}, \
    {"pu-drag-ellie", 8, 0.0f, 3.20f, -1, 1, {CD_RAW_PAIR("ellie-pullout-prone-bw--npc", "ellie-pullout-prone-bw--ellie", 1.0f, 0.0f)}}, \
    {"pu-ok-abby", 8, 0.90f, 1.50f, -1, 1, {CD_RAW_PAIR("abby-pullout-success--npc", "abby-pullout-success--abby", 1.0f, 0.0f)}}, \
    {"pu-ok-ellie", 8, 0.90f, 1.50f, -1, 1, {CD_RAW_PAIR("ellie-pullout-success--npc", "ellie-pullout-success--ellie", 1.0f, 0.0f)}}, \
    {"pu-fail-abby", 8, 1.00f, 1.75f, -1, 1, {CD_RAW_PAIR("abby-pullout-fail--npc", "abby-pullout-fail--abby", 1.0f, 0.0f)}}, \
    {"pu-fail-ellie", 8, 1.00f, 1.75f, -1, 1, {CD_RAW_PAIR("ellie-pullout-fail--npc", "ellie-pullout-fail--ellie", 1.0f, 0.0f)}}, \
    {"pu-drag-l-abby", 8, 0.0f, 2.84f, -1, 1, {CD_RAW_PAIR("abby-pullout-prone-l--npc", "abby-pullout-prone-l--abby", 1.0f, 0.0f)}}, \
    {"pu-drag-r-abby", 8, 0.0f, 2.57f, -1, 1, {CD_RAW_PAIR("abby-pullout-prone-r--npc", "abby-pullout-prone-r--abby", 1.0f, 0.0f)}}, \
    {"pu-drag-fw-abby", 8, 0.0f, 3.24f, -1, 1, {CD_RAW_PAIR("abby-pullout-prone-fw--npc", "abby-pullout-prone-fw--abby", 1.0f, 0.0f)}}, \
    {"pu-drag-l-ellie", 8, 0.0f, 2.84f, -1, 1, {CD_RAW_PAIR("ellie-pullout-prone-l--npc", "ellie-pullout-prone-l--ellie", 1.0f, 0.0f)}}, \
    {"pu-drag-r-ellie", 8, 0.0f, 2.57f, -1, 1, {CD_RAW_PAIR("ellie-pullout-prone-r--npc", "ellie-pullout-prone-r--ellie", 1.0f, 0.0f)}}, \
    {"pu-drag-fw-ellie", 8, 0.0f, 3.24f, -1, 1, {CD_RAW_PAIR("ellie-pullout-prone-fw--npc", "ellie-pullout-prone-fw--ellie", 1.0f, 0.0f)}}, \
    {"pu-grap-abby", 8, 0.0f, 1.20f, -1, 1, {CD_RAW_PAIR("abby-pullout-struggle--npc", "abby-pullout-struggle--abby", 1.0f, 0.0f)}}, \
    {"pu-grap-ellie", 8, 0.0f, 1.20f, -1, 1, {CD_RAW_PAIR("ellie-pullout-struggle--npc", "ellie-pullout-struggle--ellie", 1.0f, 0.0f)}}, \
    {"pu-kill-l", 8, 0.0f, 0.45f, -1, 1, {CD_RAW_PAIR("npc-vs-player-prone-kill-left--npc-f", "npc-vs-player-prone-kill-left--ellie", 1.0f, 0.0f)}}, \
    {"pu-kill-r", 8, 0.0f, 0.45f, -1, 1, {CD_RAW_PAIR("npc-vs-player-prone-kill-right--npc-f", "npc-vs-player-prone-kill-right--ellie", 1.0f, 0.0f)}},
