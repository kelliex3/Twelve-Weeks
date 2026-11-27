// Ported from tools/ext/pullout_sting.c. All timeline / once-only / cutoff
// rules live in pullout-sting-core.hpp; the engine speech, camera, render,
// music and side-car seams stay here. The native pull-out bridge is the only
// producer, so the ext is left alone and no wire message is added.

#include "pullout-sting.hpp"

#include "pullout-sting-pcm.hpp"

#include "boot.hpp"
#include "engine/flag.hpp"
#include "engine/pcm.hpp"
#include "engine/script.hpp"
#include "log.hpp"
#include "music-hold.hpp"
#include "net/match/net-player.hpp"
#include "net/audio/vox-effort.hpp"
#include "sid/sid.hpp"

#include <cstdint>

namespace net::audio::pullout_sting {

    namespace {

        namespace core = net::audio::pullout_sting::core;
        namespace data = net::audio::pullout_sting::data;

        constexpr char kOff[]        = "t2r-no-pullout-sting.txt";
        constexpr char kRiser[]      = "t2r-pullout-riser.txt";
        constexpr char kSpikeOff[]   = "t2r-pullout-spike-off.txt";
        constexpr char kVoxOff[]     = "t2r-pullout-vox-off.txt";
        constexpr char kSwingOff[]   = "t2r-pullout-camswing-off.txt";
        constexpr char kDofOn[]      = "t2r-pullout-dof-on.txt";
        constexpr char kVerdictVox[] = "t2r-pullout-verdict-vox.txt";

        constexpr int kRiserCutMs = 120;
        constexpr int kDecayCutMs = 60;

        core::State        g_state;
        int                g_pullerSlot = -1;
        uint64_t           g_pullerHandle = 0;
        float              g_xcam_fov_mul = 0.0f;   // native producer for the ext global
        uint64_t           g_shakeAt = 0;
        float              g_shakeAmp = 0.0f;
        bool               g_dofLogged = false;
        bool               g_gapped = false;
        engine::pcm::Voice g_riser[2];
        engine::pcm::Voice g_decay[2];

        bool ExtOwns() noexcept { return boot::ExtLoaded(); }

        bool Off()        { return engine::flag::Present(kOff); }
        bool Riser()      { return engine::flag::Present(kRiser); }
        bool SpikeOff()   { return engine::flag::Present(kSpikeOff); }
        bool VoxOff()     { return engine::flag::Present(kVoxOff); }
        bool SwingOff()   { return engine::flag::Present(kSwingOff); }
        bool DofOff()     { return !engine::flag::Present(kDofOn); }
        bool VerdictVox() { return engine::flag::Present(kVerdictVox); }

        void Gap(const char* what)
        {
            if (g_gapped)
                return;
            g_gapped = true;
            TO1_LOG("[PULLSTING] %s (native gap)\n", what);
        }

        // The bridge hands us the handle it already validated. Re-check here on
        // every DOF frame: a respawn / despawn between the grab and +2.95 s must
        // not feed a dead StringId to force-camera-dof-focus (the ext's crash).
        bool VerifiedPuppet() noexcept
        {
            if (g_pullerSlot < 0 || g_pullerSlot >= kMaxNetPlayers || !g_pullerHandle)
                return false;
            const NetPlayer& p = NetPlayers::BySlot(g_pullerSlot);
            if (!p.IsActiveRemote() || !p.spawned || p.dead || p.dying ||
                p.EngineOwned() || p.npcSid != g_pullerHandle)
                return false;
            const auto sid = static_cast<StringId64>(g_pullerHandle);
            return ENTITY::DOES_EXIST(sid) && DcManager::LookupObject(sid) != 0;
        }

        const char* LineName(core::Line line) noexcept
        {
            switch (line) {
            case core::Line::kGrabbed:    return "vox-eff-gen-meatshield-grabbed-combat";
            case core::Line::kStruggle:   return "vox-eff-gen-fight-take-med";
            case core::Line::kChoked:     return "vox-eff-gen-fight-take-lrg";
            case core::Line::kEscape:     return "vox-eff-gen-fight-give-struggle-finish";
            case core::Line::kBlow:       return "vox-eff-gen-death-melee-blunt-large";
            case core::Line::kFailDeath:  return "vox-eff-gen-death-med";
            default:                      return nullptr;
            }
        }

        void Say(core::Line line, int effort)
        {
            const char* name = LineName(line);
            if (!name)
                return;
            const uint64_t ent = SID("player");
            // fem_effort_sid: a female look resolves its own generated take by
            // kind; every other look keeps the generic SP effort line.
            uint64_t sid = effort::EffortSid(
                ent, static_cast<effort::core::Kind>(effort));
            if (!sid)
                sid = SID(name);
            const uint64_t r = DC::Call<"say">(ent, sid, 2, 0);
            TO1_LOG("[PULLSTING] say %s (%s) -> %llx\n", name,
                    effort == core::kDeath ? "death" : "effort",
                    static_cast<unsigned long long>(r));
        }

        void EngineShake(float amp)
        {
            // SP's script call; still requested first, then the measured PS32
            // curve is replayed on the victim camera by the pull-out bridge.
            const uint64_t r = DC::Call<"camera-animation-enable">(
                SID("melee-camera-shake"), SID("camera-shake-melee-hit-b"),
                0.0f, amp, 0.1f, 1, 1.0f);
            TO1_LOG("[PULLSTING] engine shake %.1f -> %llx\n", amp,
                    static_cast<unsigned long long>(r));
        }

        void MeasuredShake(uint64_t now, float amp)
        {
            g_shakeAt = now;
            g_shakeAmp = amp;
            TO1_LOG("[PULLSTING] measured shake %.1f (PS32 curve, roll 0 / yaw 0)\n", amp);
        }

        void Rumble(const char* name)
        {
            const uint64_t r = DC::Call<"start-rumble">(SID(name), 0);
            TO1_LOG("[PULLSTING] rumble %s -> %llx\n", name,
                    static_cast<unsigned long long>(r));
        }

        void Render(const core::Out& o)
        {
            const char* name = o.event == core::Event::kSpikeRender
                ? "*zgame-player-damage-struggle-spike*"
                : "*zgame-player-damage-struggle*";
            const uint64_t r = DC::Call<"override-render-settings">(
                SID(name), 0.0f, 0.0f, 0, 32767, 0, 1, 0, o.strength);
            static int nl;
            if (++nl <= 3)
                TO1_LOG("[PULLSTING] struggle %s -> %llx\n", name,
                        static_cast<unsigned long long>(r));
        }

        void Dof()
        {
            if (!VerifiedPuppet()) {
                if (!g_dofLogged)
                    Gap("no verified puller puppet for the struggle DOF");
                return;
            }
            const uint64_t r = DC::Call<"force-camera-dof-focus">(
                g_pullerHandle, SID("*zoom-camera-dof-settings-struggles*"),
                0x8b29524641f00000ULL, 1.0f, 1.0f);
            if (!g_dofLogged) {
                g_dofLogged = true;
                TO1_LOG("[PULLSTING] struggle DOF on verified puller slot %d -> %llx\n",
                        g_pullerSlot, static_cast<unsigned long long>(r));
            }
        }

        void CutPcm(engine::pcm::Voice* voices, int ms)
        {
            for (int i = 0; i < 2; ++i) {
                engine::pcm::Voice& v = voices[i];
                if (engine::pcm::Live(v))
                    engine::pcm::Cut(v, ms);
                else
                    engine::pcm::Invalidate(v);
            }
        }

        void StartPcm(bool riser)
        {
            if (!data::kHave) {
                Gap("pull-out riser / decay waves not built (TO1_PULLOUT_STING_PCM off)");
                return;
            }
            engine::pcm::Voice* voices = riser ? g_riser : g_decay;
            CutPcm(voices, 1);
            const bool ok = riser
                ? data::PlayRiser(voices[0], voices[1])
                : data::PlayDecay(voices[0], voices[1]);
            if (!ok) {
                Gap("mixer refused the pull-out riser / decay pair");
                return;
            }
            TO1_LOG("[PULLSTING] guarded PCM %s pair started\n",
                    riser ? "riser" : "decay");
        }

        void Music(core::Music music, uint64_t now)
        {
            switch (music) {
            case core::Music::kHold:
                music_hold::Hold(now + core::kHoldMs);
                break;
            case core::Music::kRiser:
                music_hold::Hold(now + core::kHoldMs);
                StartPcm(true);
                break;
            case core::Music::kEscape:
                CutPcm(g_riser, kRiserCutMs);
                music_hold::Hold(0);            // the combat loop was never stopped
                break;
            case core::Music::kFail:
                CutPcm(g_riser, kRiserCutMs);
                music_hold::Hold(now + core::kFailDecayMs);
                music_hold::NowatchUntil(now + core::kFailDecayMs);
                StartPcm(false);
                break;
            }
        }

        void Handle(const core::Result& r, uint64_t now)
        {
            for (int i = 0; i < r.count; ++i) {
                const core::Out& o = r.out[i];
                switch (o.event) {
                case core::Event::kSay:
                    Say(o.line, o.effort);
                    break;
                case core::Event::kSfx:
                    if (o.name)
                        DC::Call<"play-sound">(SID(o.name), 0);
                    break;
                case core::Event::kShake:
                    EngineShake(o.amp);
                    break;
                case core::Event::kMeasuredShake:
                    MeasuredShake(now, o.amp);
                    break;
                case core::Event::kRumble:
                    if (o.name)
                        Rumble(o.name);
                    break;
                case core::Event::kRender:
                    Render(o);
                    break;
                case core::Event::kFov:
                    g_xcam_fov_mul = o.strength;
                    break;
                case core::Event::kSpikeRender:
                    Render(o);
                    break;
                case core::Event::kDof:
                    Dof();
                    break;
                case core::Event::kMusic:
                    Music(o.music, now);
                    break;
                case core::Event::kCut:
                    music_hold::Hold(0);
                    CutPcm(g_riser, kRiserCutMs);
                    CutPcm(g_decay, kDecayCutMs);
                    g_xcam_fov_mul = 0.0f;
                    g_shakeAt = 0;
                    g_shakeAmp = 0.0f;
                    g_dofLogged = false;
                    TO1_LOG("[PULLSTING] over (%s)\n", o.why ? o.why : "?");
                    break;
                default:
                    break;
                }
            }
        }

        core::TickIn Input(uint64_t now) noexcept
        {
            core::TickIn in;
            in.now = now;
            in.voxOff = VoxOff();
            in.spikeOff = SpikeOff();
            in.dofOff = DofOff();
            in.swingOff = SwingOff();
            in.verdictVox = VerdictVox();
            return in;
        }

    } // namespace

    void OnGrab(uint64_t now, int pullerSlot, uint64_t pullerHandle)
    {
        if (ExtOwns() || Off())
            return;
        g_pullerSlot = pullerSlot;
        g_pullerHandle = pullerHandle;
        g_dofLogged = false;
        g_shakeAt = 0;
        g_shakeAmp = 0.0f;
        core::Result r;
        core::Grab(g_state, r, now, Riser());
        Handle(r, now);
        TO1_LOG("[PULLSTING] grab from slot %d - engine + measured kick, grunt in %u ms, "
                "struggle screen%s\n",
                pullerSlot, core::kGrabSayMs,
                Riser() ? ", guarded riser" : ", music held");
    }

    void OnVerdict(uint64_t now, bool ok)
    {
        if (ExtOwns() || !g_state.on)
            return;
        core::Result r;
        core::Verdict(g_state, r, now, ok, Riser(), VerdictVox());
        Handle(r, now);
        TO1_LOG("[PULLSTING] %s\n", ok ? "counter valid" : "counter failed");
    }

    void OnOutcome(uint64_t now, bool ok, float blowS)
    {
        if (ExtOwns() || !g_state.on)
            return;
        core::Result r;
        core::Outcome(g_state, r, now, ok, blowS);
        Handle(r, now);
        if (!ok)
            TO1_LOG("[PULLSTING] fail blow in %.0f ms\n", blowS * 1000.0f);
    }

    void OnTick(uint64_t now, bool dragging, bool dead, bool matchLive)
    {
        if (ExtOwns() || !g_state.on)
            return;
        if (!dragging || dead || !matchLive) {
            core::Result r;
            core::Cut(g_state, r, dead ? "dead" : dragging ? "not playing" : "pull-out over");
            Handle(r, now);
            return;
        }
        // ext refreshes g_ms_hold_until each frame while the drag is undecided;
        // after a verdict the escape / fail request owns the hold instead.
        if (!g_state.decided)
            music_hold::Hold(now + core::kHoldMs);
        core::Result r;
        core::Tick(g_state, r, Input(now));
        Handle(r, now);
    }

    void Reset()
    {
        g_state = core::State{};
        g_pullerSlot = -1;
        g_pullerHandle = 0;
        g_xcam_fov_mul = 0.0f;
        g_shakeAt = 0;
        g_shakeAmp = 0.0f;
        g_dofLogged = false;
        g_gapped = false;
        CutPcm(g_riser, 1);
        CutPcm(g_decay, 1);
    }

    bool Active() noexcept
    {
        return !ExtOwns() && g_state.on;
    }

    float FovMultiplier() noexcept
    {
        return Active() ? g_xcam_fov_mul : 0.0f;
    }

    float MeasuredShake(uint64_t now) noexcept
    {
        return Active() ? core::MeasuredPitch(now, g_shakeAt, g_shakeAmp) : 0.0f;
    }

} // namespace net::audio::pullout_sting
