// Ported from tools/ext/execsp.c. All row / timing / pick / contact rules live
// in exec-sp-core.hpp; the engine speech / joint-sound, sidecar, paired-move and
// exec-beg seams stay in this owner. No new network message.

#include "exec-sp.hpp"

#include "boot.hpp"
#include "engine/flag.hpp"
#include "exec-beg.hpp"
#include "log.hpp"
#include "engine/script.hpp"
#include "net/anim/paired.hpp"
#include "vox-effort.hpp"

#include "sid/sid.hpp"

#include <cstdint>

namespace net::audio::exec_sp {

    namespace {

        namespace core = net::audio::exec_sp::core;

        constexpr char kOff[]       = "t2r-no-exec-sp-audio.txt";
        constexpr char kVoice[]     = "t2r-exec-sp-owns-voice.txt";
        constexpr char kContacts[]  = "t2r-exec-sp-contacts.txt";

        core::State g_state;
        const char* g_name = nullptr;
        uint64_t    g_att = 0;
        uint64_t    g_vic = 0;

        bool ExtOwns() noexcept { return boot::ExtLoaded(); }
        bool Off() noexcept { return engine::flag::Present(kOff, 120); }
        bool VoiceOn() noexcept { return engine::flag::Present(kVoice, 120); }
        bool ContactsOn() noexcept { return engine::flag::Present(kContacts, 120); }

        double MoveMs() noexcept { return net::anim::paired::Ms(); }

        double Ems() noexcept
        {
            return g_state.on ? MoveMs() - g_state.t0 : 0.0;
        }

        // ext es_sfx_at. DC::Call answers ~0ULL when the native is missing; an
        // accepted handle does not prove the sound bank is loaded (the ext has
        // the same caveat).
        uint64_t PlayAt(const core::Cue& cue, uint64_t ent) noexcept
        {
            if (!ent || ent == ~0ULL || !cue.name || !cue.joint)
                return ~0ULL;
            return DC::Call<"play-sound-at-joint">(
                SID(cue.name), ent, SID(cue.joint), 0ull, 0ull);
        }

        // ext es_say_row's first-key-wins loop. A female speaker's own
        // authored effort take (ES_VOX[].vxe) overrides the generic engine key.
        bool Say(const core::Cue& cue, uint64_t ent) noexcept
        {
            if (!ent || ent == ~0ULL || cue.voice < 0
                || cue.voice >= static_cast<int>(sizeof core::kVoxKeys
                                                 / sizeof core::kVoxKeys[0]))
                return false;
            const int kind = core::kVoxKind[cue.voice];
            for (int k = 0; k < 3; ++k) {
                const char* key = core::kVoxKeys[cue.voice][k];
                if (!key)
                    break;
                // ext es_say_row resolves the female take for EACH attempted
                // key: a refused key advances fem_effort_sid's no-repeat wheel
                // before the next key is tried. Hoisting it out of the loop
                // would replay the same take on a fallback key.
                const uint64_t female = ::net::audio::effort::EffortSid(
                    ent, static_cast<::net::audio::effort::core::Kind>(kind));
                const uint64_t line = female ? female : SID(key);
                if (DC::Call<"say">(ent, line, 2, 0) != ~0ULL)
                    return true;
            }
            return false;
        }

        void Handle(const core::Result& r) noexcept
        {
            for (int i = 0; i < r.count; ++i) {
                const core::Cue& cue = r.cues[i];
                const uint64_t ent = cue.ent ? cue.ent
                    : (cue.kind == core::CueKind::kSfx ? g_att : g_vic);
                const uint64_t reply = cue.kind == core::CueKind::kSfx
                    ? PlayAt(cue, ent) : (Say(cue, ent) ? 1ull : ~0ULL);
                TO1_LOG("[EXECSP] +%.0f ms %s on %llx at %s -> %s\n",
                        cue.ems,
                        cue.kind == core::CueKind::kSfx ? "sfx" : "vox",
                        static_cast<unsigned long long>(ent),
                        cue.joint ? cue.joint : "-",
                        reply == ~0ULL ? "refused" : "accepted");
            }
        }

        void ArmPleas(const MeleeFeed& feed,
                      core::PleaRow rows[core::kMaxRowsPerPair], int n) noexcept
        {
            for (int i = 0; i < n; ++i) {
                exec_beg::MeleePleaFeed plea;
                plea.ems = feed.moveMs - feed.t0;
                plea.earliestMs = rows[i].earliestMs;
                plea.cutMs = rows[i].cutMs;
                plea.salt = rows[i].salt;
                if (!exec_beg::ScheduleMeleePlea(plea))
                    TO1_LOG("[EXECSP] %s: EST_BEG %d has no live exec_beg run\n",
                            feed.name ? feed.name : "?", i + 1);
            }
        }

    } // namespace

    void OnPairedStart(const MeleeFeed& feed)
    {
        if (ExtOwns() || Off() || !feed.name || !feed.vicEnt
            || feed.vicEnt == ~0ULL)
            return;
        if (!core::Begin(g_state, 1, feed.pair, feed.name, feed.t0, false,
                         feed.bluntOk, feed.uid))
            return;
        g_name = feed.name;
        g_att = feed.attEnt;
        g_vic = feed.vicEnt;

        core::PleaRow rows[core::kMaxRowsPerPair];
        const int n = core::Pleas(feed.name, rows);
        ArmPleas(feed, rows, n);
        TO1_LOG("[EXECSP] %s (melee pair %d, uid %x): %d rows, %d plea(s)%s\n",
                feed.name, feed.pair, static_cast<unsigned>(feed.uid),
                g_state.hi - g_state.lo, n,
                g_state.ownVic ? ", victim voice owned" : "");
    }

    bool BeginGun(const GunFeed& feed)
    {
        if (ExtOwns() || Off() || !feed.name || !feed.vicEnt
            || feed.vicEnt == ~0ULL)
            return false;
        const bool ok = core::Begin(g_state, 0, feed.pair, feed.name, feed.t0,
                                    false, true, feed.uid);
        if (!ok)
            return false;
        g_name = feed.name;
        g_att = feed.attEnt;
        g_vic = feed.vicEnt;
        TO1_LOG("[EXECSP] %s (gun pair %d, uid %x): %d rows (feed only)\n",
                feed.name, feed.pair, static_cast<unsigned>(feed.uid),
                g_state.hi - g_state.lo);
        return true;
    }

    void Contact(core::ContactLabel label)
    {
        if (ExtOwns() || Off() || !ContactsOn() || !g_state.on
            || g_state.kind != 1 || !g_name || !g_vic || g_vic == ~0ULL)
            return;
        core::Contact out;
        if (!core::ContactFor(label, g_name, Ems(), out))
            return;
        const core::Cue cue{ core::CueKind::kSfx, g_vic, out.ev, out.joint,
                             -1, Ems() };
        Handle([&] {
            core::Result r;
            core::AddCue(r, cue);
            return r;
        }());
    }

    void ShiftClock(double fromT0, double deltaMs)
    {
        if (ExtOwns())
            return;
        core::Shift(g_state, fromT0, deltaMs);
    }

    void Tick()
    {
        if (ExtOwns() || !g_state.on)
            return;

        const net::anim::paired::State& ps = net::anim::paired::Get();
        if (g_state.uid && (ps.role == 0 || ps.uid != g_state.uid)) {
            TO1_LOG("[EXECSP] stopped (paired move ended) after %d layer(s)\n",
                    g_state.played);
            core::Cut(g_state);
            g_name = nullptr;
            g_att = g_vic = 0;
            return;
        }

        const double ems = Ems();
        core::TickInput in;
        in.moveMs = MoveMs();
        in.attEnt = g_att;
        in.vicEnt = g_vic;
        // ext: only a non-owning timeline yields victim rows to a gun plea.
        in.gunVoiceCovered = !g_state.ownVic && exec_beg::CoversGun(ems);
        Handle(core::Tick(g_state, in));
    }

    void Reset()
    {
        if (ExtOwns())
            return;
        core::Reset(g_state);
        g_name = nullptr;
        g_att = g_vic = 0;
    }

    bool Active() noexcept
    {
        if (ExtOwns())
            return false;
        return g_state.on;
    }

    bool HasPleas(const char* name) noexcept
    {
        if (ExtOwns())
            return false;
        return core::HasPleas(name);
    }

    bool HoldsVictimVoice() noexcept
    {
        if (ExtOwns() || !g_state.on || !g_state.ownVic || !VoiceOn())
            return false;
        return Ems() < core::kOwnsVoiceMs;
    }

} // namespace net::audio::exec_sp
