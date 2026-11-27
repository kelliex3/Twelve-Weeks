// Ported from tools/ext/exec_beg.c. All timing / pick / cut rules live in
// exec-beg-core.hpp; the engine speech, sidecar, paired-move and music-duck
// seams stay in this owner. No new network message.

#include "exec-beg.hpp"

#include "exec-beg-data.hpp"
#include "exec-sp.hpp"

#include "boot.hpp"
#include "downed-vox.hpp"
#include "engine/flag.hpp"
#include "engine/script.hpp"
#include "exec-music.hpp"
#include "gameplay/melee2013.hpp"
#include "log.hpp"
#include "net/anim/paired.hpp"
#include "net/game/player-status.hpp"
#include "net/protocol/net-protocol.hpp"
#include "vox-effort.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace net::audio::exec_beg {

    namespace {

        namespace core = net::audio::exec_beg::core;
        namespace data = net::audio::exec_beg::data;

        constexpr char kOff[]          = "t2r-no-exec-beg.txt";
        constexpr char kMeleeOff[]     = "t2r-no-melee-beg.txt";
        constexpr char kKeepScream[]   = "t2r-exec-keep-long-scream.txt";
        constexpr char kNoPleaBoost[]  = "t2r-no-plea-boost.txt";
        constexpr char kMeleeBoost[]   = "t2r-melee-plea-boost.txt";
        constexpr char kFinVoicesOff[] = "t2r-fin-voices-ps30.txt";

        core::State g_state;
        uint32_t    g_activeUid = 0;              // paired-move dedupe key
        uint64_t    g_finLast[kMaxNetPlayers + 1] = {};

        bool ExtOwns() noexcept { return boot::ExtLoaded(); }
        bool Off()             { return engine::flag::Present(kOff, 120); }
        bool MeleeOff()        { return engine::flag::Present(kMeleeOff, 120); }
        bool KeepScream()      { return engine::flag::Present(kKeepScream, 120); }
        bool PleaBoostOff()    { return engine::flag::Present(kNoPleaBoost, 120); }
        bool MeleeBoostOk()    { return engine::flag::Present(kMeleeBoost, 120); }
        bool FinVoicesOff()    { return engine::flag::Present(kFinVoicesOff, 120); }

        // The ext used the native vox_stop helper. The mod probes the same DC
        // names downed-vox uses, because UC4 renamed that native on some builds.
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
            if (!ent)
                return ~0ULL;
            static int pick = -2;               // -2 probe, -1 none, 0..3 name
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

        bool SayRecord(const core::Cue& cue)
        {
            if (!cue.line)
                return false;
            const uint64_t r = DC::Call<"say">(cue.ent, cue.line, 2, 0);
            TO1_LOG("[EXECBEG] say %llx record %llx -> %llx\n",
                    static_cast<unsigned long long>(cue.ent),
                    static_cast<unsigned long long>(cue.line),
                    static_cast<unsigned long long>(r));
            return r != ~0ULL;
        }

        bool SayEvent(const core::Cue& cue)
        {
            if (cue.voiceKind < 0 || cue.voiceKind > 7)
                return false;
            for (int i = 0; i < 3; ++i) {
                const char* name = core::kEffortEvents[cue.voiceKind][i];
                if (!name)
                    break;
                const uint64_t effort = ::net::audio::effort::EffortSid(
                    cue.ent, ::net::audio::effort::core::Kind::kPain);
                const uint64_t sid = effort ? effort : SID(name);
                const uint64_t r = DC::Call<"say">(cue.ent, sid, 2, 0);
                TO1_LOG("[EXECBEG] effort event %s on %llx -> %llx\n",
                        name, static_cast<unsigned long long>(cue.ent),
                        static_cast<unsigned long long>(r));
                if (r != ~0ULL)
                    return true;
            }
            return false;
        }

        void Handle(const core::Result& r)
        {
            if (r.duckChanged) {
                exec_music::SetDuck(r.duckWant ? core::kMusicDuck : 1.0f);
                TO1_LOG("[EXECBEG] execution music %s (duck %.2f)\n",
                        r.duckWant ? "ducked under the plea" : "back up",
                        static_cast<double>(r.duckWant ? core::kMusicDuck : 1.0f));
            }
            if (r.boost)
                DC::Call<"set-per-frame-mixer-override">(core::kPleaSnap);

            bool hadSpeak = false;
            bool spoke = false;
            for (int i = 0; i < r.count; ++i) {
                const core::Cue& cue = r.cues[i];
                switch (cue.kind) {
                case core::CueKind::kSpeakRecord:
                    hadSpeak = true;
                    spoke = SayRecord(cue) || spoke;
                    break;
                case core::CueKind::kSpeakEvent:
                    hadSpeak = true;
                    spoke = SayEvent(cue) || spoke;
                    break;
                case core::CueKind::kStop: {
                    const uint64_t cut = StopVoice(cue.ent);
                    TO1_LOG("[EXECBEG] stop-vox on %llx -> %llx\n",
                            static_cast<unsigned long long>(cue.ent),
                            static_cast<unsigned long long>(cut));
                    break;
                }
                case core::CueKind::kHeadshot:
                    // Ext: the death shot into the head uses SP's headshot death
                    // take (downed_vox.c), not a body-shot scream.
                    downed_vox::ArmHeadshot(cue.ent, true, 0);
                    TO1_LOG("[EXECBEG] death shot headshot take on %llx\n",
                            static_cast<unsigned long long>(cue.ent));
                    break;
                default:
                    break;
                }
            }
            if (hadSpeak)
                core::CommitSpeak(g_state, spoke);
            if (r.cutWhy)
                TO1_LOG("[EXECBEG] stopped (%s)\n", r.cutWhy);
        }

        uint32_t MeleeSeed(const PairedStart& start, int pair)
        {
            const uint32_t vs = start.vicSlot >= 0
                ? static_cast<uint32_t>(start.vicSlot) : 0u;
            return (vs + 1u) * 0x2545f491u
                 ^ (static_cast<uint32_t>(pair) + 1u) * 0x9e3779b1u
                 ^ start.uid * 0x85ebca6bu;
        }

        void GapOnce(const char* what)
        {
            static bool logged = false;
            if (logged)
                return;
            logged = true;
            TO1_LOG("[EXECBEG] %s (native gap: no gun-execution producer / voice mapping)\n",
                    what);
        }

    } // namespace

    void OnPairedStart(const PairedStart& start, bool armPlea)
    {
        if (ExtOwns() || Off() || MeleeOff() || !start.vicEnt
            || start.vicEnt == ~0ULL)
            return;

        if (start.kind == net::anim::paired::kGunExec) {
            // No gun-execution producer, shot schedule, aim table or third-party
            // state exists natively; BeginGun() is the feed for a future one.
            GapOnce("gun execution waiting for its shot schedule");
            return;
        }
        if (start.kind == net::anim::paired::kFinisher) {
            FinisherFeed feed;
            feed.ent = start.vicEnt;
            feed.slot = start.vicSlot >= 0 ? static_cast<uint32_t>(start.vicSlot) : 0u;
            feed.seed = MeleeSeed(start, -1);
            feed.why = "paired finisher";
            FinGasp(feed);
            return;
        }
        if (start.kind != net::anim::paired::kMeleeExec)
            return;

        const int pair = gameplay::melee2013::ByMove(start.move);
        if (pair < 0) {
            GapOnce("melee execution with an unknown 2013 move");
            return;
        }

        core::MeleeBegin in;
        in.ent = start.vicEnt;
        in.pair = pair;
        // The native paired state carries no faction / look / voice mapping.
        // The effort-event fallback is live with fac/gender/voice unknown.
        in.fac = -1;
        in.female = -1;
        in.voice = -1;
        in.seed = MeleeSeed(start, pair);
        in.moveMs = net::anim::paired::Ms();
        if (!core::BeginMelee(g_state, in, false, false, KeepScream(), true))
            return;
        g_activeUid = start.uid;

        float killS = start.killS > 0.0f
            ? start.killS : gameplay::melee2013::At(pair).killS;
        const double cutMs = killS > 0.0 ? static_cast<double>(killS) * 1000.0 : 700.0;
        if (!armPlea)
            return;                     // exec_sp owns this pair's EST_BEG rows
        core::Result r;
        // No authored EST_BEG rows for this pair: the move's kill frame is the
        // only blow moment available, so one short plea is armed at the start
        // and cut there (the ext's effect with its first row at the first blow).
        core::BeginMeleePlea(g_state, r, data::Table(), GetTickCount64(),
                             0.0, cutMs, core::kMeleePleaSalt);
        Handle(r);
        TO1_LOG("[EXECBEG] melee plea armed on %llx for %s at %.0f ms (effort events; "
                "no faction / look / voice mapping)\n",
                static_cast<unsigned long long>(start.vicEnt),
                gameplay::melee2013::At(pair).name ? gameplay::melee2013::At(pair).name : "?",
                cutMs);
    }

    bool ScheduleMeleePlea(const MeleePleaFeed& feed)
    {
        if (ExtOwns() || !g_state.on || g_state.nshot > 0)
            return false;
        core::Result r;
        if (!core::ScheduleMeleePlea(g_state, r, data::Table(), feed.ems,
                                     feed.earliestMs, feed.cutMs, feed.salt))
            return false;
        Handle(r);
        return true;
    }

    bool BeginGun(const GunFeed& feed)
    {
        if (ExtOwns())
            return false;
        if (Off() || !feed.ent || feed.ent == ~0ULL || !feed.shots
            || feed.shotCount <= 0 || feed.fac < 0 || feed.female < 0
            || feed.voice < 0 || !data::kHave) {
            GapOnce("gun execution feed not ready");
            return false;
        }
        core::GunBegin in;
        in.ent = feed.ent;
        in.pair = feed.pair;
        in.fac = feed.fac;
        in.female = feed.female;
        in.voice = feed.voice;
        in.seed = feed.seed;
        in.moveMs = feed.moveMs;
        in.skipMs = feed.skipMs;
        in.weaponClass = feed.weaponClass;
        in.shots = feed.shots;
        in.shotCount = feed.shotCount;
        in.limb = feed.limb;
        in.lastHead = feed.lastHead;
        const bool ok = core::BeginGun(g_state, data::Table(), in, false,
                                       KeepScream(), false);
        g_activeUid = 0;                 // no paired move owns this gun feed
        if (ok)
            TO1_LOG("[EXECBEG] gun plea armed on %llx, %d shot(s)\n",
                    static_cast<unsigned long long>(feed.ent), feed.shotCount);
        return ok;
    }

    void FinGasp(const FinisherFeed& feed)
    {
        if (ExtOwns() || Off() || FinVoicesOff() || !feed.ent
            || feed.ent == ~0ULL || !net::player_status::MatchLive())
            return;
        const uint32_t li = feed.ent == SID("player")
            ? static_cast<uint32_t>(kMaxNetPlayers) : feed.slot;
        if (li > static_cast<uint32_t>(kMaxNetPlayers))
            return;
        const uint64_t now = GetTickCount64();
        if (g_finLast[li] && now - g_finLast[li] < 3000)
            return;                         // once per finisher
        if (g_state.on) {
            TO1_LOG("[EXECBEG] no gasp (%s) - an execution owns the victim voice\n",
                    feed.why ? feed.why : "?");
            return;
        }
        if (feed.fac < 0 || feed.fac > 1 || feed.female < 0 || feed.female > 1
            || feed.voice < 0 || feed.voice > 3) {
            GapOnce("finisher gasp waiting for the voice mapping");
            return;
        }
        const int rec = core::FinGaspPick(data::Table(), feed.fac, feed.female,
                                          feed.voice, feed.seed);
        g_finLast[li] = now;
        uint64_t sid = 0;
        if (rec >= 0 && !g_state.events)
            sid = data::Line(rec);
        if (!sid)
            sid = ::net::audio::effort::EffortSid(
                feed.ent, ::net::audio::effort::core::Kind::kPain);
        if (!sid) {
            // Same engine fallback as the ext when no authored take answers.
            sid = SID("vox-eff-gen-begs-for-life-stun");
        }
        const uint64_t r = DC::Call<"say">(feed.ent, sid, 2, 0);
        TO1_LOG("[EXECBEG] fin gasp on %llx (%s): %llx -> %llx\n",
                static_cast<unsigned long long>(feed.ent),
                feed.why ? feed.why : "?", static_cast<unsigned long long>(sid),
                static_cast<unsigned long long>(r));
    }

    void Tick()
    {
        if (ExtOwns())
            return;

        const net::anim::paired::State& ps = net::anim::paired::Get();
        if (g_state.on && g_activeUid != 0
            && (ps.role == 0 || ps.uid != g_activeUid)) {
            core::Result ended;
            core::Cut(g_state, ended, "paired move ended");
            Handle(ended);
        }

        const uint64_t now = GetTickCount64();
        uint64_t checkReply = ~0ULL;
        if (core::CheckDue(g_state, now))
            checkReply = DC::Call<"is-speaking?">(g_state.ent);

        core::TickInput in;
        in.nowMs = now;
        in.moveMs = net::anim::paired::Ms();
        in.matchLive = net::player_status::MatchLive();
        in.pleaBoostOff = PleaBoostOff();
        in.meleeBoostAllowed = MeleeBoostOk();
        in.musicLive = exec_music::Playing() && !exec_music::Fading();
        in.checkReply = checkReply;
        in.table = data::Table();
        Handle(core::Tick(g_state, in));
    }

    void Reset()
    {
        if (ExtOwns())
            return;
        const bool ducked = g_state.ducked;
        core::Reset(g_state);
        g_activeUid = 0;
        std::memset(g_finLast, 0, sizeof g_finLast);
        if (ducked)
            exec_music::SetDuck(1.0f);
    }

    bool Active() noexcept
    {
        if (ExtOwns())
            return false;
        return g_state.on;
    }

    bool CoversGun(double ems) noexcept
    {
        if (ExtOwns())
            return false;
        return core::CoversGun(g_state, ems);
    }

    bool Covers(double ems) noexcept
    {
        if (ExtOwns())
            return false;
        return CoversGun(ems) || exec_sp::HoldsVictimVoice();
    }

} // namespace net::audio::exec_beg
