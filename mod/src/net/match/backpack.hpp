#pragma once

#include <cstdint>

// Ported from tools/ext/backpack.c: the TAB backpack (crafting / shop) as other players
// see it. The own side (the worn bag <-> held prop swap inside the engine's own
// EnterBackpack / LeaveBackpack) lives in engine/backpack-prop.*; this file is the wire +
// the per-frame driver:
//
//   * the owner broadcasts its backpack phase (MSG_BACKPACK = NEVT_BACKPACK 0x5B, BP_VER 1):
//     [u8 ver][u8 phase][u8 bagoff][u8 seq][u32 ms in phase][u16 enter ms][u16 leave ms].
//     phase 0 idle, 1 open (intro -> loop), 2 closing (outro), 3 abort (death / down /
//     match end: blend out now). Sent on every change (+100 / +300 ms copies), every 500 ms
//     while open / closing and every 3 s while idle, so a lost packet or a late joiner
//     converges within one period.
//   * every receiver takes that player's bag off the puppet: the 23 worn parts are hidden
//     and a held bag prop follows the puppet's right hand through the intro / outro and
//     rests on the floor during the crouch loop. Never zero bags: the worn bag goes off only
//     once the prop object exists.
//   * the recorded prop track (MSG_BACKPACK ver 0x54 spinec / ver 0x55 root chunks): while
//     OUR prop is out the prop's joints are sampled per clip (1 intro, 2 loop, 3 outro) and
//     25 ms step relative to the body, saved next to the exe and streamed to the peers, so
//     the bag on another player follows that player's own recorded body-relative path.
//
// The puppet's own BODY craft clip is NOT played here: the mod's anim replication
// (net/anim) already replays the owner's captured AnimCmdList, so the craft clip is on the
// puppet from there. This module only drives the PROP and the worn-bag visibility, and uses
// the craft clip's length as the clip clock (the ext's BpPupPlay / BpPupSync, without the
// body AnimateFrom). The animated TAB prop (mode 1) is spawned through the engine's own
// ProcessBackpack spawn (BpSpawnReal) with the ext's bp_pick detour honouring the forced art;
// the ext's per-look faction-look prop tables stay gated (the engine's own art is used), and
// the rigid per-look bag / satchel (opt-in t2r-puppet-bag-rigid.txt) is the fallback.
//
// Opt-outs next to the exe: t2r-no-puppet-bag-off.txt (the old behaviour: the puppet keeps
// its worn bag), t2r-puppet-bag-rigid.txt (opt in to the rigid bag / satchel), and the track
// files t2r-no-bp-track.txt / t2r-bp-record.txt.

namespace net::match::backpack {

    // boot::FrameTick, once per frame: the own phase machine, the own-side bag hand-off poll
    // and the per-puppet prop / clip tick.
    void Tick();

    // net::replication::HandleNetEvent, NEVT_BACKPACK. The ext routes all three message
    // shapes (the 12-byte phase, the 0x54 spinec chunk, the 0x55 root chunk) through one
    // byte, so this dispatches on data[0].
    void OnEvent(uint8_t senderSlot, const uint8_t* data, uint8_t size);

    // Session teardown / leave the match: drop every per-slot puppet state and the track.
    void Reset();

}
