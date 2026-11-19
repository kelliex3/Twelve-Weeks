// ported from tools/ext/paired.c (the g_ps surface bloodbuild reads; the wire / placement are
// not ported -- the mod's own execution system drives the halves)

#include "paired.hpp"

#include "engine/frame.hpp"

namespace net::anim::paired {

    namespace {

        State  g_ps;            // ext g_ps (the subset above)
        uint32_t g_counter;     // ext g_psCounter

        AbortFn g_abortFn;      // ext paired.c: psync_abort -> the consumer's cancel (BbCancelPair)

    }

    const State& Get()
    {
        return g_ps;
    }

    double Ms()
    {
        return engine::frame::NowMs();
    }

    bool Active()
    {
        return g_ps.role != 0;
    }

    void BeginAttacker(int attSlot, int vicSlot, uint64_t vicSid, uint64_t weapon, uint64_t move, float killS)
    {
        g_ps = State{};
        g_ps.role = 1;
        g_ps.kind = kMeleeExec;
        g_ps.uid = ++g_counter;
        g_ps.att = attSlot;             // our own slot (ext g_ps.att)
        g_ps.vic = vicSlot;
        g_ps.entAtt = SID("player");
        g_ps.entVic = vicSid;
        g_ps.flags = kFLethal;          // the mod's execution kills the victim
        g_ps.weapon = weapon;
        g_ps.move = move;
        g_ps.killS = killS;
    }

    void BeginVictim(int attSlot, int vicSlot, uint64_t attSid, uint64_t move, float killS)
    {
        g_ps = State{};
        g_ps.role = 2;
        g_ps.kind = kMeleeExec;
        g_ps.uid = ++g_counter;
        g_ps.att = attSlot;
        g_ps.vic = vicSlot;             // our own slot (ext g_ps.vic)
        g_ps.entAtt = attSid;
        g_ps.entVic = SID("player");
        g_ps.flags = kFLethal;          // we are being executed: the attacker's half kills us
        g_ps.weapon = 0;                // not carried on the sync event
        g_ps.move = move;
        g_ps.killS = killS;
    }

    void End(bool aborted)
    {
        if (aborted && g_abortFn)
            g_abortFn(g_ps.att, g_ps.vic);
        g_ps.role = 0;
        g_ps.kind = 0;
        g_ps.att = -1;
        g_ps.vic = -1;
        g_ps.entAtt = 0;
        g_ps.entVic = 0;
        g_ps.flags = 0;
        g_ps.weapon = 0;
        g_ps.move = 0;
        g_ps.killS = 0.0f;
        // uid is kept: a consumer dedupes on it and a cleared uid must not re-fire the same
        // move. The next Begin* bumps it.
    }

    void SetAbortHandler(AbortFn fn)
    {
        g_abortFn = fn;
    }

    void Reset()
    {
        g_ps = State{};
        g_counter = 0;
    }

}
