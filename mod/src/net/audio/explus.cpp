// Ported from tools/ext/explus.c. All timeline / mixer / recipe / voice rules
// live in explus-core.hpp; the engine audio, speech, sidecar, private-data and
// paired-move seams stay in this owner. No new network message.

#include "explus.hpp"

#include "explus-data.hpp"

#include "boot.hpp"
#include "engine/exe_side.hpp"
#include "engine/flag.hpp"
#include "engine/object.hpp"
#include "engine/pcm.hpp"
#include "exec-beg.hpp"
#include "intfoley-core.hpp"
#include "log.hpp"
#include "engine/script.hpp"
#include "net/anim/paired.hpp"
#include "net/match/net-player.hpp"
#include "vox-effort.hpp"

#include "sid/sid.hpp"

#include <windows.h>

#include <cstdio>
#include <cstdint>

namespace net::audio::explus {

    namespace {

        namespace core = net::audio::explus::core;
        namespace data = net::audio::explus::data;

        constexpr char kOff[]        = "t2r-no-exec-foley-plus.txt";
        constexpr char kHandSlaps[]  = "t2r-explus-hand-slaps.txt";
        constexpr char kFistRub[]    = "t2r-explus-fist-rub.txt";
        constexpr char kStompClaps[] = "t2r-explus-stomp-claps.txt";
        constexpr uint64_t kFileCacheMs = 2000;

        core::State g_state;
        const char* g_name = nullptr;
        uint32_t    g_uid = 0;
        uint64_t    g_att = 0, g_vic = 0;
        bool        g_shiv = false, g_vicLine = false, g_voxClash = false, g_vicDying = false;

        bool ExtOwns() noexcept { return boot::ExtLoaded(); }
        bool Off() noexcept { return engine::flag::Present(kOff, 120); }
        bool OptIn(const char* file) noexcept { return engine::flag::Present(file, 120); }

        uint64_t Now() noexcept { return GetTickCount64(); }
        double MoveMs() noexcept { return net::anim::paired::Ms(); }

        float CachedPercent(const char* file, float def, float scale, bool plus) noexcept
        {
            static uint64_t last[2] = {};
            static float value[2] = { 0.45f, 0.70f };
            const int i = plus ? 1 : 0;
            const uint64_t now = Now();
            if (now - last[i] > kFileCacheMs) {
                last[i] = now;
                value[i] = def;
                if (FILE* f = engine::exe_side::Open(file, "r")) {
                    int pc = 0;
                    if (std::fscanf(f, "%d", &pc) == 1 && pc >= 0 && pc <= 400)
                        value[i] = scale * static_cast<float>(pc) / 100.0f;
                    std::fclose(f);
                }
            }
            return value[i];
        }

        float Lead() noexcept { return CachedPercent("t2r-melee-sfx-gain.txt", 0.45f, 1.0f, false); }
        float Plus() noexcept { return CachedPercent("t2r-exec-foley-plus-gain.txt", 0.70f, 0.70f, true); }

        // Faction cloth: WLF (team 1) is leather / military, everyone else is
        // cotton / parka. Unknown bodies stay cotton, as in ext ep_material().
        int Material(uint64_t ent) noexcept
        {
            if (!ent || ent == ~0ULL)
                return 0;
            if (ent == SID("player")) {
                const NetPlayer* p = NetPlayers::Local();
                return p && p->team == 1 ? 1 : 0;
            }
            for (const NetPlayer& p : NetPlayers::Remotes()) {
                if (p.UserSid() == ent || p.npcSid == ent)
                    return p.team == 1 ? 1 : 0;
            }
            return 0;
        }

        // A generated row at a known joint is a one-shot world voice there; a
        // row without a joint answer follows the body. The 30 m near range
        // neutralizes the mixer's default 3-30 m curve: the tier / layer gain
        // computed by the core is the only distance model.
        void Play(const core::Cue& cue) noexcept
        {
            if (cue.kind != core::Cue::Kind::kPcm || cue.samples <= 0)
                return;
            const engine::pcm::Sample* src = data::Frames(cue.wave, cue.foot);
            if (!src)
                return;
            float pos[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1};
            const char* joint = core::JointName(cue.joint);
            if (cue.ent && cue.ent != ~0ULL && joint
                && engine::object::JointLocator(cue.ent, joint, pos, q)) {
                engine::pcm::PlayAtRange(src, cue.samples, cue.gain,
                                         static_cast<int>(cue.rate), pos, 30.0f, 30.1f);
            }
            else if (cue.ent && cue.ent != ~0ULL) {
                engine::pcm::PlayAt(src, cue.samples, cue.gain,
                                    static_cast<int>(cue.rate), cue.ent);
            }
        }

        void Say(const core::Cue& cue) noexcept
        {
            int count = 0, vxe = 0;
            const char* const* pool = core::VoxPool(cue.layer, count, vxe);
            if (!pool || cue.eventIndex < 0 || cue.eventIndex >= count
                || !cue.ent || cue.ent == ~0ULL)
                return;
            // ext's VoxPool vxe 0/1 = VXE_GIVE / VXE_PAIN. A female speaker
            // plays her own authored take; otherwise the engine resolves the
            // generic key's default gender.
            const auto kind = vxe == 1
                ? ::net::audio::effort::core::Kind::kPain
                : ::net::audio::effort::core::Kind::kGive;
            const uint64_t now = Now();
            // ext ep_vox tries the pool from its rotation until a key answers,
            // resolving fem_effort_sid on EVERY attempt: a refused key marks
            // that take bad and advances the no-repeat wheel before the next
            // key is tried. The core has already picked the first clean key;
            // continue from there so a refusal still gets ext's same fallback.
            for (int t = 0; t < count; ++t) {
                const int k = (cue.eventIndex + t) % count;
                if (!pool[k] || core::BadVoice(g_state, cue.layer, k))
                    continue;
                const uint64_t female = ::net::audio::effort::EffortSid(cue.ent, kind);
                const uint64_t line = female ? female : SID(pool[k]);
                const uint64_t reply = DC::Call<"say">(cue.ent, line, 2, 0);
                core::ReportVoice(g_state, cue.layer, cue.who, k,
                                  reply != ~0ULL, now);
                if (reply != ~0ULL)
                    return;
            }
        }

        void Handle(const core::Result& r) noexcept
        {
            for (int i = 0; i < r.count; ++i) {
                if (r.cues[i].kind == core::Cue::Kind::kPcm)
                    Play(r.cues[i]);
                else
                    Say(r.cues[i]);
            }
        }

        core::TickInput Input() noexcept
        {
            core::TickInput in;
            in.nowMs = static_cast<double>(Now());
            in.ems = g_state.on ? MoveMs() - g_state.t0 : 0.0;
            in.level = Lead() * Plus();
            in.activeVoices = engine::pcm::ActiveVoices();
            in.attEnt = g_att;
            in.vicEnt = g_vic;
            in.off = Off();
            in.handSlaps = OptIn(kHandSlaps);
            in.fistRub = OptIn(kFistRub);
            in.stompClaps = OptIn(kStompClaps);
            in.shivActive = g_shiv;
            in.vicLine = g_vicLine;
            in.ebCovers = exec_beg::Covers(in.ems);
            in.voxClash = g_voxClash;
            in.lead = Lead();
            in.plus = Plus();
            return in;
        }

        core::RecipeInput RecipeInput() noexcept
        {
            core::RecipeInput in;
            in.nowMs = static_cast<double>(Now());
            in.level = Lead() * Plus();
            in.activeVoices = engine::pcm::ActiveVoices();
            in.off = Off();
            in.shivActive = g_shiv;
            in.vicLine = g_vicLine;
            in.ebCovers = false;   // queued recipes have no exec_beg clock window
            in.voxClash = g_voxClash;
            in.lead = Lead();
            in.plus = Plus();
            return in;
        }

        void Run(const core::Recipe& rec, uint64_t att, uint64_t vic) noexcept
        {
            if (ExtOwns() || Off() || !data::Have || !rec.rows)
                return;
            core::Result out;
            core::QueueRecipe(g_state, data::Data(), rec, att, vic,
                              Material(att), Material(vic), RecipeInput(), out);
            Handle(out);
        }

        bool Begin(const PairFeed& feed) noexcept
        {
            if (ExtOwns() || Off() || !feed.name || !feed.vicEnt
                || feed.vicEnt == ~0ULL || !data::Have)
                return false;
            const bool gated = intfoley::core::Gate(feed.kind, feed.name,
                engine::flag::Present("t2r-no-int-foley.txt", 120));
            const bool ok = core::Begin(g_state, data::Data(), feed.kind,
                feed.pair, feed.name, feed.t0,
                feed.attMat >= 0 ? feed.attMat : Material(feed.attEnt),
                feed.vicMat >= 0 ? feed.vicMat : Material(feed.vicEnt),
                false, gated);
            if (!ok)
                return false;
            g_name = feed.name;
            g_uid = feed.uid;
            g_att = feed.attEnt;
            g_vic = feed.vicEnt;
            g_state.vicDying = g_vicDying;
            TO1_LOG("[EXPLUS] %s (kind %d pair %d): %d rows, cloth %d/%d%s\n",
                    feed.name, feed.kind, feed.pair, g_state.hi - g_state.lo,
                    g_state.mat[0], g_state.mat[1],
                    data::Have ? "" : " (no private tables)");
            return true;
        }

    } // namespace

    void OnPairedStart(const PairFeed& feed)
    {
        (void)Begin(feed);
    }

    bool BeginGun(const PairFeed& feed)
    {
        if (feed.kind != 0)
            return false;
        return Begin(feed);
    }

    void ContextAct(int act, int style, uint64_t att, uint64_t vic)
    {
        Run(core::ContextRecipe(act, style), att, vic);
        if (act == 3 && style != -1)
            Run(core::HeavyContextRecipe(style), att, vic);
    }

    void FinisherLabel(int label, uint64_t att)
    {
        Run(core::FinisherRecipe(label), att, 0);
    }

    void DownedCue(int kind, int cls, uint64_t att, uint64_t vic)
    {
        Run(core::DownedRecipe(kind, cls), att, vic);
    }

    void IntFoleyCue(int k, bool fist, uint64_t att, uint64_t vic)
    {
        Run(core::IntFoleyRecipe(k, fist), att, vic);
    }

    void SetShivActive(bool on) noexcept { g_shiv = on; }
    void SetVicLine(bool on) noexcept { g_vicLine = on; }
    void SetVoxClash(bool on) noexcept { g_voxClash = on; }
    void SetVictimDying(bool on) noexcept
    {
        g_vicDying = on;
        g_state.vicDying = on && g_state.on;
    }

    void ShiftClock(double fromT0, double deltaMs)
    {
        if (ExtOwns() || !g_state.on || g_state.t0 != fromT0)
            return;
        g_state.t0 += deltaMs;
    }

    void Tick()
    {
        if (ExtOwns() || Off())
            return;
        core::Result out;
        if (g_state.on) {
            const net::anim::paired::State& ps = net::anim::paired::Get();
            if (g_uid && (ps.role == 0 || ps.uid != g_uid)) {
                Cut("paired-move ended");
                return;
            }
            const core::TickInput in = Input();
            core::Tick(g_state, data::Data(), in, out);
            Handle(out);
        }
        core::Result queued;
        core::TickQueue(g_state, data::Data(), static_cast<double>(Now()),
                        RecipeInput(), queued);
        Handle(queued);
    }

    void Cut(const char* why)
    {
        if (ExtOwns())
            return;
        if (g_state.on)
            TO1_LOG("[EXPLUS] cut (%s) after %d row(s)\n", why ? why : "?", g_state.played);
        core::Reset(g_state);
        g_name = nullptr;
        g_uid = 0;
        g_att = g_vic = 0;
        g_shiv = g_vicLine = g_voxClash = false;
    }

    void Reset()
    {
        if (ExtOwns())
            return;
        core::Reset(g_state);
        g_name = nullptr;
        g_uid = 0;
        g_att = g_vic = 0;
        g_shiv = g_vicLine = g_voxClash = g_vicDying = false;
    }

    bool Active() noexcept
    {
        return !ExtOwns() && g_state.on;
    }

} // namespace net::audio::explus
