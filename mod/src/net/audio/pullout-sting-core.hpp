#pragma once

// Pure prone-pull-out sting rules (tools/ext/pullout_sting.c): the grab /
// struggle / fail timeline, the 150 ms grabbed grunt, the SP effort beats, the
// success escape line, the fail blow's camera kick + spike screen, the victim
// FOV swing and the music requests. No engine, DC, sidecar, network or private
// PCM header is included here; the native owner in pullout-sting.cpp supplies
// those seams. Tests: mod/tests/pullout_sting_core_test.cpp.
//
// The owner passes the sidecar answers and the match / drag state in as flags,
// so every timing / once-only / cutoff decision stays here.

#include <cstdint>

namespace net::audio::pullout_sting::core {

    inline constexpr uint32_t kHoldMs      = 900;
    inline constexpr uint32_t kGrabSayMs   = 150;
    inline constexpr uint32_t kVox1Ms      = 900;
    inline constexpr uint32_t kVox2Ms      = 2500;
    inline constexpr uint32_t kDofAtMs     = 2950;
    inline constexpr uint32_t kSpikeMs     = 900;
    inline constexpr uint32_t kEscapeMs    = 300;
    inline constexpr uint32_t kBlowMinMs   = 50;
    inline constexpr uint32_t kFailDecayMs = 4400;   // SP set decay, empty this long after the blow
    inline constexpr float    kSwingS      = 2.7f;
    inline constexpr float    kFov0        = 0.9498f;
    inline constexpr float    kGameFov     = 73.74f;   // ext XCAM_T2_DEFAULT_FOV

    // ext PS32 MFK32 (tools/ext/melee_feel.c): the measured camera-shake-hit-b
    // pitch, in degrees per unit scale at 60 Hz. Pull-out calls mfk_add with
    // roll=0, pitch=1, yaw=0, so this column is the whole replayed curve; the
    // engine clip is still requested separately by the owner.
    inline constexpr int      kMeasuredN   = 27;
    inline constexpr float    kMeasuredPitch[kMeasuredN] = {
         0.00f,  0.02f,  0.20f,  0.23f,  0.23f, -0.06f,
        -0.39f, -0.77f, -1.13f, -1.22f, -1.31f, -1.11f,
        -0.89f, -0.66f, -0.40f, -0.28f, -0.17f, -0.11f,
        -0.05f, -0.03f, -0.02f,  0.00f,  0.00f,  0.00f,
         0.00f,  0.00f,  0.00f,
    };

    // ext VXE_ effort kinds (shared with vox-effort-core.hpp).
    inline constexpr int kGive  = 0;
    inline constexpr int kPain  = 1;
    inline constexpr int kDeath = 2;

    enum class Line : int {
        kGrabbed = 0,          // vox-eff-gen-meatshield-grabbed-combat
        kStruggle,             // infected-struggle / fight-take-med
        kChoked,               // player-choked-enter / fight-take-lrg
        kEscape,               // infected-struggle-escape / give-struggle-finish
        kBlow,                 // death-melee-blunt-large / (verdict vox) take-lrg-blnt
        kFailDeath,            // optional verdict-vox death line
        kCount,
    };

    enum class Event : int {
        kNone = 0,
        kSay,            // `line` + `effort`
        kSfx,            // `name` (static literal)
        kShake,          // engine clip, `amp`
        kMeasuredShake,  // PS32 curve replay alongside the clip, `amp`
        kRumble,         // `name`
        kRender,         // struggle screen, `strength`
        kFov,            // victim lens multiplier, `strength`
        kSpikeRender,    // struggle spike, `strength`
        kDof,            // puller focus, re-requested every struggle frame
        kMusic,          // `music`
        kCut,            // `why` copied by the owner
    };

    enum class Music : int { kHold = 0, kEscape, kFail, kRiser };

    struct Out {
        Event       event = Event::kNone;
        Line        line = Line::kGrabbed;
        Music       music = Music::kHold;
        int         effort = kPain;
        float       amp = 0.0f;
        float       strength = 1.0f;
        const char* name = nullptr;
        const char* why = nullptr;
    };

    struct Result {
        Out   out[12];
        int   count = 0;
        bool  end = false;

        void Add(Event event) noexcept
        {
            if (count < 12)
                out[count++] = Out{ event };
        }
        void Say(Line line, int effort) noexcept
        {
            if (count >= 12) return;
            Out& o = out[count++];
            o.event = Event::kSay;
            o.line = line;
            o.effort = effort;
        }
        void Sfx(const char* name) noexcept
        {
            if (count >= 12) return;
            Out& o = out[count++];
            o.event = Event::kSfx;
            o.name = name;
        }
        void Shake(float amp) noexcept
        {
            if (count >= 12) return;
            Out& o = out[count++];
            o.event = Event::kShake;
            o.amp = amp;
        }
        void MeasuredShake(float amp) noexcept
        {
            if (count >= 12) return;
            Out& o = out[count++];
            o.event = Event::kMeasuredShake;
            o.amp = amp;
        }
        void Rumble(const char* name) noexcept
        {
            if (count >= 12) return;
            Out& o = out[count++];
            o.event = Event::kRumble;
            o.name = name;
        }
        void Render(Event event, float strength) noexcept
        {
            if (count >= 12) return;
            Out& o = out[count++];
            o.event = event;
            o.strength = strength;
        }
        void Dof() noexcept { Add(Event::kDof); }
        void MusicRequest(Music music) noexcept
        {
            if (count >= 12) return;
            Out& o = out[count++];
            o.event = Event::kMusic;
            o.music = music;
        }
        void Cut(const char* why) noexcept
        {
            if (count >= 12) return;
            Out& o = out[count++];
            o.event = Event::kCut;
            o.why = why;
            end = true;
        }
    };

    struct State {
        bool     on = false;
        bool     decided = false;
        bool     escapeDone = false;
        bool     blowDone = false;
        bool     dofOn = false;
        int      voxN = 0;
        uint64_t t0 = 0;
        uint64_t grabSayAt = 0;
        uint64_t escapeAt = 0;
        uint64_t blowAt = 0;
        uint64_t spikeUntil = 0;
    };

    inline void Reset(State& s) noexcept { s = State{}; }

    // ext SP lens: smoothstep from 70 deg (0.9498 of the game FOV) to the game
    // FOV over the 2.7 s swing, held afterwards. `off` is the sidecar opt-out.
    inline float FovMul(uint64_t now, uint64_t t0, bool off) noexcept
    {
        if (off)
            return 0.0f;
        uint64_t el = now > t0 ? now - t0 : 0;
        float u = static_cast<float>(el) / (kSwingS * 1000.0f);
        if (u > 1.0f) u = 1.0f;
        u = u * u * (3.0f - 2.0f * u);
        return kFov0 + (1.0f - kFov0) * u;
    }

    // ext mfk_rot's PS32 row interpolation. `startMs` is the request frame; the
    // returned value is the pitch to ADD to the victim camera pose in degrees
    // (negative = view down here, matching QuatYawPitch's positive-up sign).
    inline float MeasuredPitch(uint64_t now, uint64_t startMs, float amp) noexcept
    {
        if (!(amp > 0.0f) || now < startMs)
            return 0.0f;
        const double frame = static_cast<double>(now - startMs) * 60.0 / 1000.0;
        if (frame >= static_cast<double>(kMeasuredN - 1))
            return 0.0f;
        const int k = static_cast<int>(frame);
        const float u = static_cast<float>(frame - static_cast<double>(k));
        const float a = kMeasuredPitch[k];
        return (a + (kMeasuredPitch[k + 1] - a) * u) * amp;
    }

    inline uint64_t BlowAt(uint64_t now, float blowS) noexcept
    {
        if (blowS < 0.05f) blowS = 0.05f;
        const double ms = static_cast<double>(blowS) * 1000.0;
        return now + (ms > static_cast<double>(kBlowMinMs)
                      ? static_cast<uint64_t>(ms) : kBlowMinMs);
    }

    // ext pullsting_grab: kick + rumble, the grabbed grunt at +150 ms, and the
    // music request (hold, or the opt-in riser whose clips are not in this tree).
    inline void Grab(State& s, Result& out, uint64_t now, bool riser) noexcept
    {
        Reset(s);
        s.on = true;
        s.t0 = now;
        s.grabSayAt = now + kGrabSayMs;
        out.Shake(1.7f);
        out.MeasuredShake(1.7f);
        out.Rumble("melee-standing-finisher-strong-rumble-settings");
        out.MusicRequest(riser ? Music::kRiser : Music::kHold);
    }

    // ext pullsting_result: on a valid counter the combat loop stays and the
    // escape grunt lands 300 ms later; on a fail the death-pullout sfx plays
    // (and, with the old opt-in, a verdict death line) and the set goes to decay.
    inline void Verdict(State& s, Result& out, uint64_t now, bool ok,
                        bool riser, bool verdictVox) noexcept
    {
        if (!s.on || s.decided)
            return;
        s.decided = true;
        s.dofOn = false;
        if (ok) {
            s.escapeAt = now + kEscapeMs;
            out.MusicRequest(Music::kEscape);
        } else {
            out.Sfx("sfx-ellie-death-pullout-fail");
            if (verdictVox)
                out.Say(Line::kFailDeath, kDeath);
            out.MusicRequest(Music::kFail);
        }
        (void)riser;   // only the owner's log / missing riser sink cares
    }

    // ext pullsting_outcome: only a fail has a blow; the native bridge derives
    // `blowS` from its authored kBlowMs (no victim-side timestamp is replicated).
    inline void Outcome(State& s, Result&, uint64_t now, bool ok,
                        float blowS) noexcept
    {
        if (!s.on || ok)
            return;
        s.blowAt = BlowAt(now, blowS);
    }

    struct TickIn {
        uint64_t now = 0;
        bool     voxOff = false;
        bool     spikeOff = false;
        bool     dofOff = true;     // ext's DOF is opt-in until the handle is known
        bool     swingOff = false;
        bool     verdictVox = false;
    };

    // ext pullsting_tick: every-frame struggle override, SP vox beats, lens,
    // fail blow, spike screen and the puller's DOF.
    inline void Tick(State& s, Result& out, const TickIn& in) noexcept
    {
        if (!s.on)
            return;
        const uint64_t el = in.now > s.t0 ? in.now - s.t0 : 0;

        if (s.grabSayAt && in.now >= s.grabSayAt) {
            s.grabSayAt = 0;
            if (!in.voxOff)
                out.Say(Line::kGrabbed, kPain);
        }
        if (!in.voxOff) {
            if (s.voxN == 0 && el >= kVox1Ms) {
                s.voxN = 1;
                out.Say(Line::kStruggle, kPain);
            }
            if (s.voxN == 1 && el >= kVox2Ms) {
                s.voxN = 2;
                out.Say(Line::kChoked, kPain);
            }
            if (s.escapeAt && in.now >= s.escapeAt) {
                s.escapeAt = 0;
                s.escapeDone = true;
                out.Say(Line::kEscape, kGive);
            }
        }

        out.Render(Event::kRender, 1.0f);
        const float fov = FovMul(in.now, s.t0, in.swingOff);
        if (fov > 0.0f)
            out.Render(Event::kFov, fov);

        if (s.blowAt && in.now >= s.blowAt && !s.blowDone) {
            s.blowDone = true;
            s.blowAt = 0;
            if (!in.spikeOff) {
                s.spikeUntil = in.now + kSpikeMs;
                out.Shake(2.2f);
                out.MeasuredShake(2.2f);
                if (!in.voxOff)
                    out.Say(Line::kBlow, in.verdictVox ? kPain : kDeath);
            }
        }
        if (s.spikeUntil && in.now < s.spikeUntil && !in.spikeOff)
            out.Render(Event::kSpikeRender, 1.0f);

        // SP requests the focus every frame of the struggle clips, not once.
        if (!in.dofOff && !s.decided && el >= kDofAtMs) {
            s.dofOn = true;
            out.Dof();
        }
    }

    // ext pullsting_cut: verdict-less drag end, death, match exit or reset.
    inline void Cut(State& s, Result& out, const char* why) noexcept
    {
        if (!s.on)
            return;
        Reset(s);
        out.Cut(why ? why : "?");
    }

} // namespace net::audio::pullout_sting::core
