// Ported from tools/ext/intvox.c. All pick / history / timeline / cut / check
// rules live in intvox-core.hpp; the engine speech, look-at, slam, paired-move,
// sidecar, voice and foley seams stay in this owner. No new network message.

#include "intvox.hpp"

#include "intvox-data.hpp"

#include "intfoley.hpp"
#include "vox-effort-core.hpp"
#include "vox-effort.hpp"

#include "boot.hpp"
#include "engine/flag.hpp"
#include "engine/script.hpp"
#include "gameplay/melee2013.hpp"
#include "log.hpp"
#include "net/anim/paired.hpp"
#include "net/match/net-player.hpp"
#include "net/protocol/net-protocol.hpp"
#include "net/ui/customize-compose.hpp"
#include "sid/sid.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace net::audio::intvox {

    namespace {

        namespace core   = net::audio::intvox::core;
        namespace data   = net::audio::intvox::data;
        namespace effort = net::audio::effort;

        constexpr char kOff[]      = "t2r-no-int-vox.txt";
        constexpr char kLookOff[]  = "t2r-no-int-look.txt";
        constexpr char kSlamOff[]  = "t2r-no-int-slam-ps4.txt";
        constexpr char kQuietWhy[] = "interrogation voices";

        core::State g_state;
        uint32_t    g_uid = 0;               // paired-move dedupe key
        uint64_t    g_quietUntil = 0;
        uint64_t    g_nextLook = 0;
        int         g_lookN[2] = { 0, 0 };
        bool        g_gapped = false;

        bool ExtOwns() noexcept { return boot::ExtLoaded(); }
        bool Off()             { return engine::flag::Present(kOff, 120); }
        bool LookOff()         { return engine::flag::Present(kLookOff, 120); }
        bool OldSlams()        { return engine::flag::Present(kSlamOff, 120); }

        void Gap(const char* what)
        {
            if (g_gapped)
                return;
            g_gapped = true;
            TO1_LOG("[INTVOX] %s (native gap)\n", what);
        }

        // The ext's vox_stop helper. UC4 renamed it on some builds, so probe
        // the same names exec-beg / downed-vox use and cache the answer.
        uint64_t CallStop(int which, uint64_t ent)
        {
            switch (which) {
            case 0: return DC::Call<"stop-vox">(ent);
            case 1: return DC::Call<"stop-speaking">(ent);
            case 2: return DC::Call<"stop-speaking-interruptible">(ent);
            case 3: return DC::Call<"stop-line">(ent);
            default: return ~0ULL;
            }
        }

        uint64_t StopVoice(uint64_t ent)
        {
            if (!ent || ent == ~0ULL)
                return ~0ULL;
            static int pick = -2;
            if (pick == -2) {
                pick = -1;
                for (int i = 0; i < 4; ++i) {
                    const uint64_t r = CallStop(i, ent);
                    if (r != ~0ULL) {
                        pick = i;
                        return r;
                    }
                }
                return ~0ULL;
            }
            return pick < 0 ? ~0ULL : CallStop(pick, ent);
        }

        int OwnFaction() noexcept
        {
            const NetPlayer* p = NetPlayers::Local();
            if (!p || (p->team != 0 && p->team != 1))
                return -1;
            return p->team;                    // ext vx_own_fac: 0 Seraphite / 1 WLF
        }

        // ext ivx_voice: the look THIS body wears on this screen, its faction
        // from the roster team and its voice letter from the composed body (or
        // the look's authored letter). A body we cannot resolve stays silent
        // and its beats use effort events.
        bool VoiceFor(uint64_t ent, int& fac, int& female, int& voice) noexcept
        {
            if (!ent || ent == ~0ULL)
                return false;
            const NetPlayer* local = NetPlayers::Local();
            const bool mine = ent == SID("player") || (local && local->npcSid == ent);
            const int own = OwnFaction();
            if (own < 0)
                return false;

            int body = -1;
            const char* look = nullptr;
            if (mine) {
                if (!local)
                    return false;
                fac = own;
                look = local->LookOrDefault();
                body = static_cast<int>(kMaxNetPlayers);
            } else {
                for (const NetPlayer& p : NetPlayers::Remotes()) {
                    if (p.npcSid != ent)
                        continue;
                    fac = NetPlayers::SlotIsFriendly(p.netSlot) ? own : !own;
                    look = p.LookOrDefault();
                    body = p.netSlot;
                    break;
                }
                if (body < 0 || !look)
                    return false;
            }
            if (!look || !*look)
                return false;

            const int li = effort::core::LookIndex(look);
            female = effort::core::FemaleLook(look) ? 1 : 0;
            voice = net::ui::customize::compose::BodyVoice(body, fac);
            if (voice < 0 && li >= 0)
                voice = effort::core::VoiceOfLook(fac, li);
            if (voice < 0 || voice > 3 || fac < 0 || fac > 1)
                return false;
            return true;
        }

        uint64_t SayRecord(const core::Cue& cue)
        {
            const uint64_t ent = g_state.ent[cue.who];
            if (!ent || ent == ~0ULL)
                return ~0ULL;
            // The ext stops the victim unconditionally before every line (and
            // any priority-26 retry): is-speaking? cannot see the paired clip's
            // effort events, so "silent" before the call proves nothing.
            if (cue.who == 1 || cue.priority >= 26)
                StopVoice(ent);
            const core::VoxTable t = data::Table();
            const uint64_t line = t.Line(cue.rec);
            if (!line)
                return ~0ULL;
            const uint64_t r = DC::Call<"say">(ent, line, cue.priority, 0);
            TO1_LOG("[INTVOX] say %llx rec %llx prio %d -> %llx\n",
                    static_cast<unsigned long long>(ent),
                    static_cast<unsigned long long>(line), cue.priority,
                    static_cast<unsigned long long>(r));
            return r;
        }

        uint64_t SayEvent(const core::Cue& cue)
        {
            const uint64_t ent = g_state.ent[cue.who];
            if (!ent || ent == ~0ULL)
                return ~0ULL;
            const effort::core::Kind kind = cue.who == 0
                ? effort::core::Kind::kGive : effort::core::Kind::kPain;
            uint64_t r = ~0ULL;
            for (int i = 0; i < 3 && r == ~0ULL; ++i) {
                const char* name = core::EffortEvent(cue.who, cue.kindNo, i);
                if (!name)
                    break;
                uint64_t sid = i == 0 ? effort::EffortSid(ent, kind) : 0;
                if (!sid)
                    sid = SID(name);
                r = DC::Call<"say">(ent, sid, 2, 0);
                TO1_LOG("[INTVOX] effort %s on %llx -> %llx\n", name,
                        static_cast<unsigned long long>(ent),
                        static_cast<unsigned long long>(r));
            }
            return r;
        }

        void SlamHit(const core::Cue& cue)
        {
            const uint64_t ent = g_state.ent[1];
            if (!ent || ent == ~0ULL || !OldSlams())
                return;
            static constexpr const char* const kEvents[2] = {
                "sfx-unarmed-finisher-headslam-skin", "sfx-wall-impact-sweetener"
            };
            for (const char* name : kEvents) {
                const uint64_t r = DC::Call<"play-sound-at-joint">(
                    SID(name), ent, SID("headb"), 0, 0);
                TO1_LOG("[INTVOX] old slam %s%s -> %llx\n", name,
                        cue.second ? " (final)" : "",
                        static_cast<unsigned long long>(r));
            }
            Gap("head-slash blood / 2013 impact wave need native producers");
        }

        void LookSet(uint64_t now, double ems)
        {
            const double secs = (g_state.killMs - ems) / 1000.0 + 1.0;
            const float dur = static_cast<float>(secs < 1.0 ? 1.0 : secs);
            for (int w = 1; w >= 0; --w) {       // the victim's eyes first
                const uint64_t ent = g_state.ent[w], tgt = g_state.ent[1 - w];
                if (!ent || ent == ~0ULL || !tgt || tgt == ~0ULL)
                    continue;
                const uint64_t r = DC::Call<"npc-look-at-object">(
                    ent, tgt, SID("headb"), dur);
                uint64_t mode = ~0ULL;
                if (r != ~0ULL && !g_lookN[w]) {
                    mode = DC::Call<"npc-set-look-aim-mode">(ent, 1);
                }
                ++g_lookN[w];
                TO1_LOG("[INTVOX] look %d -> %d for %.1f s: %llx / %llx\n", w, 1 - w,
                        static_cast<double>(dur), static_cast<unsigned long long>(r),
                        static_cast<unsigned long long>(mode));
            }
            g_nextLook = now + 600;
        }

        void LookClear()
        {
            for (int w = 0; w < 2; ++w) {
                if (!g_lookN[w])
                    continue;
                const uint64_t ent = g_state.ent[w];
                if (ent && ent != ~0ULL) {
                    const uint64_t r = DC::Call<"npc-clear-look-at">(ent);
                    TO1_LOG("[INTVOX] clear look %d -> %llx\n", w,
                            static_cast<unsigned long long>(r));
                }
                g_lookN[w] = 0;
            }
            g_nextLook = 0;
        }

        void FeedFoley(const core::LineFeed& line)
        {
            intfoley::core::LineFeed feed;
            feed.on = line.on;
            feed.t0 = line.t0;
            feed.shift = line.shift;
            feed.step = line.step;
            feed.count = line.count < 8 ? line.count : 8;
            for (int w = 0; w < 2; ++w) {
                feed.busy[w] = line.busy[w];
                feed.vline[w] = line.vline[w];
            }
            for (int i = 0; i < feed.count; ++i)
                feed.steps[i].at = line.steps[i].at;
            intfoley::SetLine(feed);
        }

        void Handle(const core::Result& r, uint64_t now, double ems)
        {
            for (int i = 0; i < r.count; ++i) {
                const core::Cue& cue = r.cues[i];
                switch (cue.kind) {
                case core::CueKind::kSayRecord:
                    if (cue.priority >= 26)
                        StopVoice(g_state.ent[cue.who]);
                    SayRecord(cue);
                    break;
                case core::CueKind::kSayEffort:
                    SayEvent(cue);
                    break;
                case core::CueKind::kSlamGive:
                case core::CueKind::kSlamTake:
                    SayEvent(cue);
                    break;
                case core::CueKind::kSlamHit:
                    SlamHit(cue);
                    break;
                case core::CueKind::kStopVictim: {
                    const uint64_t cut = StopVoice(g_state.ent[cue.who]);
                    TO1_LOG("[INTVOX] stop %llx on %llx -> %llx\n",
                            static_cast<unsigned long long>(g_state.ent[cue.who]),
                            static_cast<unsigned long long>(cue.who),
                            static_cast<unsigned long long>(cut));
                    break;
                }
                default:
                    break;
                }
            }
            if (r.quietUntilSet)
                g_quietUntil = r.quietUntil;
            if (r.look && !LookOff() && now >= g_nextLook)
                LookSet(now, ems);
            if (r.lookClear)
                LookClear();
            if (r.end)
                TO1_LOG("[INTVOX] end (%s)\n", r.why ? r.why : "?");
            FeedFoley(r.line);
        }

        void Begin(const net::anim::paired::State& ps, uint64_t now)
        {
            const int pair = gameplay::melee2013::ByMove(ps.move);
            if (pair < 0)
                return;
            const gameplay::melee2013::Move& mv = gameplay::melee2013::At(pair);
            const bool fist = mv.kind == gameplay::melee2013::kInterrogate;
            const bool shiv = mv.kind == gameplay::melee2013::kIntShiv;
            if (!fist && !shiv)
                return;

            core::BeginIn in;
            in.t0 = net::anim::paired::Ms();
            in.killMs = mv.killS > 0.0f ? static_cast<double>(mv.killS) * 1000.0
                                       : (fist ? 4600.0 : 2250.0);
            in.impactMs = 2267.0;
            in.swingMs = 4300.0;
            in.smashMs = 5000.0;
            in.warped = false;                 // the native move is not slowed
            in.fist = fist;
            in.uid = ps.uid;
            in.pair = pair;
            in.attSlot = ps.att;
            in.vicSlot = ps.vic;
            in.ent[0] = ps.entAtt;
            in.ent[1] = ps.entVic;
            for (int w = 0; w < 2; ++w) {
                const int slot = w == 0 ? ps.att : ps.vic;
                in.ok[w] = VoiceFor(in.ent[w], in.fac[w], in.female[w], in.voice[w]);
                if (!in.ok[w])
                    Gap("no faction / look / voice set for an interrogation body");
                TO1_LOG("[INTVOX] %s %s voice fac %d %s voice %d (slot %d)\n",
                        w ? "victim" : "interrogator", in.ok[w] ? "own" : "unknown",
                        in.fac[w], in.female[w] ? "female" : "male", in.voice[w], slot);
            }

            core::Begin(g_state, data::Table(), in);
            g_uid = ps.uid;
            g_nextLook = 0;
            g_lookN[0] = g_lookN[1] = 0;
            TO1_LOG("[INTVOX] %s %s: kill at %.0f ms, %d steps, seed %08x\n",
                    mv.name ? mv.name : "?", fist ? "fist" : "shiv", in.killMs,
                    g_state.nstep, g_state.seed);
            (void)now;
        }

    } // namespace

    void Tick()
    {
        if (ExtOwns())
            return;

        const net::anim::paired::State& ps = net::anim::paired::Get();
        if (g_state.on && (ps.role == 0 || ps.uid != g_uid)) {
            core::Result ended;
            core::Cut(g_state, ended, "paired interrogation ended");
            Handle(ended, ::GetTickCount64(), 0.0);
            g_uid = 0;
        }
        if (Off()) {
            FeedFoley(core::LineFeed{});
            return;
        }
        if (!g_state.on && ps.role != 0)
            Begin(ps, ::GetTickCount64());

        const uint64_t now = ::GetTickCount64();
        bool speaking = false;
        bool checkDue = false;
        if (core::CheckDue(g_state, now)) {
            speaking = (DC::Call<"is-speaking?">(g_state.ent[g_state.checkW]) & 0xff) != 0;
            checkDue = true;
        }
        const double ems = g_state.on
            ? net::anim::paired::Ms() - g_state.t0 : 0.0;
        core::TickIn in;
        in.ems = ems;
        in.t0 = g_state.t0;
        in.now = now;
        in.speaking = speaking;
        in.checkDue = checkDue;
        in.lookOff = LookOff();
        core::Result r;
        core::Tick(g_state, r, data::Table(), in);
        Handle(r, now, ems);
    }

    void Reset()
    {
        core::Reset(g_state);
        g_uid = 0;
        g_quietUntil = 0;
        g_nextLook = 0;
        g_lookN[0] = g_lookN[1] = 0;
        g_gapped = false;
        FeedFoley(core::LineFeed{});
    }

    bool Active() noexcept
    {
        return !ExtOwns() && g_state.on != 0;
    }

    bool Quiet(uint64_t now) noexcept
    {
        return !ExtOwns() && now < g_quietUntil;
    }

} // namespace net::audio::intvox
