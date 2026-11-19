#pragma once

#include <cstdint>

// Ported from tools/ext/bloodbuild.c: characters get bloodier and dirtier during a life.
// TLOU2 single-player does not switch to a bloody mesh variant -- it stamps a per-character
// BLOOD MASK render target (shader sampler BloodMaskSampler) with `bm-*` particle groups the
// takedown clips fire as `part-effect` anim events (engine::clipfx), by hit zone. This feature
// keeps a per-body blood / dirt level driven by events every client already sees identically
// (synced health, paired moves, down / death, the downed crawl, the life counter) and escalates
// into the engine's own blood-mask / particle effects.
//
// It is purely local and cosmetic: no network message of its own. Because the moves it reacts
// to (paired executions / takedowns, hits, downs) are the same on every screen, the built-up
// blood matches on every screen without anything on the wire.
//
// The mask / spurt rows come from three private generated tables (tools/ext/GENERATED.md:
// ctx_fx_events.h -> CTX_FX / KNIFE_HIT_FX, finisher_anims.h -> FIN_ANIMS). Those generators
// are not shipped, so the tables cannot be produced locally; without them the feature keeps its
// faithful early-out (`BbInit` finds no rows -> the whole tick is inert). Define
// T2R_DATA_CTX_FX and drop the generated headers in the include path to make it live.
//
// Opt-outs next to the exe (ext): t2r-no-blood-buildup.txt = off, t2r-no-blood-dirt.txt =
// blood only, t2r-no-shiv-blood.txt, t2r-no-int-blood.txt, t2r-blood-melee-cuts.txt (old
// melee-cut placement), t2r-bb-abort-keep.txt, t2r-melee-down-revivable.txt.

namespace net::match::bloodbuild {

    // The per-frame driver (ext BloodbuildTick). Reads the local player and every spawned
    // puppet, escalates the levels and fires the due effects (at most 2 per frame).
    void Tick();

    // A body just took a MELEE blow: record it so a wound within 1.2 s is stamped where SP's
    // blunt / fist clips stamp their victim (the head), never a torso knife cut (ext
    // BbMeleeNote). `who` is a slot 0..kMaxNetPlayers-1, or kMaxNetPlayers for ourselves.
    // `blade` = a blade dealt the blow. Called by the (unported) melee hit-reaction / duel-pin
    // features; until they land the "melee cuts" branch stays inert.
    void NoteMeleeBlow(int who, bool blade);

    // The Interrogation head slams (ext BbIgSlam, called by the (unported) intvox feature):
    // `second` 0 = the slam into the ground (a small hit), 1 = the final smash (the whole
    // head-smash effect set). Returns the number of part-effects queued.
    int  IgSlam(uint64_t ent, int second);

    // Gun-execution gore (ext GxbWound / GxbPool, called by the (unported) exec-gore feature):
    // a shot stamps a wound mask at once; the killing shot leaves SP's blood pool under the
    // corpse ~1.4 s later. `who` is a slot 0..kMaxNetPlayers-1 or kMaxNetPlayers for ourselves.
    void GunWound(int who, int zone);
    void GunPool(uint64_t ent, int who, double delayMs);

    // Match reset / disconnect: clear every body's state and the queued effects.
    void Reset();

}
