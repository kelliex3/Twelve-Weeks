#pragma once

#include "sid/sid.hpp"

#include <cstdint>

// Ported from tools/ext/paired.c's `g_ps` -- the psync paired-move surface, restricted to the
// fields a per-client cosmetic feature reads: which paired move (if any) is playing here, on
// which two bodies, how lethal it is and when it kills. The ext's full psync (the wire, the
// per-half placement, the puppet gates) is NOT ported: the mod runs its own execution system
// (gameplay::melee) and this module is the read-only view of it that other features consume,
// plus the tiny bridge the execution system writes.
//
// Paired moves are executions / finishers / tackles / revives / contextual downs: a move both
// clients play at once. Because every client runs the same move, a feature that reacts to it
// needs no network message of its own -- it reads the same state on every screen.

namespace net::anim::paired {

    // ext PS_GUN_EXEC .. PS_CTXDOWN.
    enum Kind {
        kGunExec   = 1,
        kMeleeExec = 2,
        kFinisher  = 3,
        kTackle    = 4,
        kRevive    = 5,
        kCtxdown   = 6,
    };

    // ext PS_F_LETHAL: the victim's client kills the victim at killS (owner-authorised death).
    inline constexpr uint32_t kFLethal = 0x01;

    // ext `g_ps` -- the fields bloodbuild (and the like) read. The ext's role 3 (third party)
    // is never produced here: the mod tracks only our own two roles.
    struct State {
        int      role = 0;      // 0 idle, 1 attacker (we started it), 2 victim (we are being executed)
        int      kind = 0;      // Kind
        uint32_t uid  = 0;      // changes once per move (dedupe key; ext: initiator slot << 24 | counter)
        int      att = -1;      // attacker slot (-1 none)
        int      vic = -1;      // victim slot (-1 none)
        uint64_t entAtt = 0;    // SID("player") or a puppet handle playing the attacker half here
        uint64_t entVic = 0;    // SID("player") or a puppet handle playing the victim half here
        uint32_t flags = 0;     // kF* bits
        uint64_t weapon = 0;    // the attacker's held weapon
        uint64_t move = 0;      // the melee move id (a t1mx-* 2013 id, or the mod's action sid)
        float    killS = 0.0f;  // move time of the kill / effect (s); <= 0 = none known here
    };

    // The current paired move (ext g_ps). role == 0 when none.
    const State& Get();

    // ext PsyncMs: a monotonic QPC millisecond clock, the same one engine::frame::NowMs returns.
    double Ms();

    // A paired move is playing on us (either half).
    bool Active();

    // ---- the bridge (gameplay::melee) ---------------------------------------------------
    // The mod's execution system calls these at the move's start / end. They only fill the
    // State above; they do not drive any animation.

    // We are the attacker: our body plays the attacker half, `vicSid` is the victim puppet.
    // `attSlot` is our own slot (the ext tracks it in g_ps.att; -1 when unknown).
    void BeginAttacker(int attSlot, int vicSlot, uint64_t vicSid, uint64_t weapon,
                       uint64_t move, float killS);

    // We are the victim: `attSid` is the attacker puppet, our body plays the victim half.
    // `vicSlot` is our own slot (the ext tracks it in g_ps.vic; -1 when unknown).
    void BeginVictim(int attSlot, int vicSlot, uint64_t attSid, uint64_t move, float killS);

    // The move is over here (either role). `aborted` is true when it ended before its kill
    // frame (a dodge / a gone puppet / a timeout), so a consumer can cancel what it queued
    // for the move -- the ext's paired.c calls the abort handler (psync_abort) in that case.
    void End(bool aborted = false);

    // A consumer (bloodbuild) registers a callback for an aborted pair; it is called with the
    // two body slots just before the State is cleared. The ext hard-wires BbCancelPair here;
    // the mod keeps gameplay/melee decoupled from the feature by using this hook.
    using AbortFn = void (*)(int att, int vic);
    void SetAbortHandler(AbortFn fn);

    // Clear the whole state (match reset / disconnect).
    void Reset();

}
