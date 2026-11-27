// Ported from tools/ext/intfoley.c. Window / gap / no-double / voice-yield
// rules live in intfoley-core.hpp; the paired-move, sidecar and explus sinks stay
// in this owner. No new network message.

#include "intfoley.hpp"

#include "explus-data.hpp"
#include "explus.hpp"

#include "boot.hpp"
#include "engine/flag.hpp"
#include "gameplay/melee2013.hpp"
#include "log.hpp"
#include "net/anim/paired.hpp"

#include <cstdint>

namespace net::audio::intfoley {

    namespace {

        namespace core = net::audio::intfoley::core;

        constexpr char kOff[]     = "t2r-no-int-foley.txt";
        constexpr char kExOff[]   = "t2r-no-exec-foley-plus.txt";

        core::State    g_state;
        core::LineFeed g_line;                 // inert until intvox is ported
        uint32_t       g_uid = 0;
        double         g_t0 = 0.0;
        bool           g_logged = false;

        bool ExtOwns() noexcept { return boot::ExtLoaded(); }
        bool Off() noexcept
        {
            return engine::flag::Present(kOff, 120)
                || engine::flag::Present(kExOff, 120);
        }

        double MoveMs() noexcept { return net::anim::paired::Ms(); }

    } // namespace

    void SetLine(const core::LineFeed& line)
    {
        g_line = line;
    }

    void Tick()
    {
        const net::anim::paired::State& ps = net::anim::paired::Get();
        if (ExtOwns() || ps.role == 0 || ps.kind != net::anim::paired::kMeleeExec) {
            Reset();
            return;
        }

        const int idx = gameplay::melee2013::ByMove(ps.move);
        const char* name = idx >= 0 ? gameplay::melee2013::At(idx).name : nullptr;
        if (!name || core::Pair(name) == core::PairKind::kNone) {
            Reset();
            return;
        }
        if (Off()) {
            core::Reset(g_state);
            g_uid = 0;
            return;
        }

        if (!g_state.on || g_uid != ps.uid) {
            if (g_uid == ps.uid)                 // this run was cut / finished
                return;
            g_uid = ps.uid;
            g_t0 = MoveMs();
            core::Begin(g_state, 1, name, g_t0,
                        static_cast<double>(gameplay::melee2013::At(idx).killS) * 1000.0,
                        ps.entAtt, ps.entVic);
            if (!g_logged) {
                g_logged = true;
                TO1_LOG("[INTFOLEY] %s: shaking window %.0f..%.0f ms, "
                        "voices yield to intvox (no native feed: inert)\n",
                        name, g_state.a, g_state.b);
            }
        }

        core::Result out;
        core::Tick(g_state, MoveMs() - g_t0, net::audio::explus::data::Data(),
                   idx, g_line, Off(), out);
        for (int i = 0; i < out.count; ++i) {
            const core::Cue& cue = out.cues[i];
            net::audio::explus::IntFoleyCue(cue.k, cue.fist, cue.att, cue.vic);
        }
    }

    void Reset()
    {
        core::Reset(g_state);
        g_line = core::LineFeed{};
        g_uid = 0;
        g_t0 = 0.0;
    }

} // namespace net::audio::intfoley
