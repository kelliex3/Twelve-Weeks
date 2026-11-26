#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Pure core of tools/ext/choose.c + side_tick's CHOOSE YOUR SIDE stage machine
// (build 191). No game headers, no Windows: shared by the native owner and the
// standalone harness in mod/tests/choose_core_test.cpp. Camera writes, mixer
// voices, vox, pages and widget images are owner seams -- what stays here is
// the stage transitions, focus / hover decisions and the authored tweens.
namespace net::ui::choose::core {

    // stage values from side_tick (t2r-ext.c): 0 intro card, 1 faction pick,
    // 2 crossfade into the pick, 3 exit fade, 4 popped, 6 select payoff,
    // 10 the end-screen branch (owned by the postmatch module).
    enum Stage : int {
        kIntro     = 0,
        kPick      = 1,
        kCrossfade = 2,
        kFadeOut   = 3,
        kPopped    = 4,
        kPayoff    = 6,
        kEndScreen = 10,
    };

    // side_tick (t2r-ext.c): SIDE_IN_MS / SIDE_STAGGER_MS / SIDE_OUT_MS /
    // SIDE_PICK_OUT_MS / SIDE_BG_A. The page crossfade and its entrance / exit
    // ramps use side_ease(), not the tween's ce_ease().
    constexpr uint64_t kInMs          = 560;
    constexpr uint64_t kStaggerMs     = 140;
    constexpr uint64_t kOutMs         = 300;
    constexpr uint64_t kPickOutMs     = 1800;
    constexpr float    kBgA           = 0.90f;

    constexpr float    kZoom          = 2.0f;
    constexpr float    kZoomMs        = 900.0f;
    constexpr float    kPanMs         = 800.0f;
    constexpr float    kNameMs        = 350.0f;
    constexpr float    kFadeOutMs     = 130.0f;
    constexpr float    kFadeInMs      = 250.0f;
    constexpr float    kTabY          = 1250.0f;
    constexpr float    kTabK          = 0.40f;
    constexpr float    kBigX          = 1920.0f;
    constexpr float    kBigY          = 1365.0f;
    constexpr float    kLogoUp        = 1.35f;
    constexpr float    kLogoDown      = 0.72f;
    constexpr float    kDolly         = 3.0f;
    constexpr float    kTruck         = 2.5f;
    constexpr float    kGy            = 560.0f;
    constexpr float    kLogoY         = 1095.0f;
    constexpr float    kLogoCX[2]     = { 1320.0f, 2520.0f };
    constexpr float    kColHW         = 480.0f;
    constexpr float    kColY0         = 940.0f;
    constexpr float    kColY1         = 1330.0f;
    constexpr float    kHlMs          = 180.0f;
    constexpr float    kHlLogo        = 0.12f;
    constexpr float    kHlDim         = 0.55f;
    constexpr float    kGrainMs       = 125.0f;
    constexpr float    kBrandOutMs    = 900.0f;
    constexpr uint64_t kSelectGateMs  = 350;   // CH_NAME_MS: a click selects only after the focus settled

    // The key-art camera model (choose.c + clanend.c): the five layer depths,
    // CE_FPX, and the tarp whose lift sets the intro's start height.
    constexpr float kFpx        = 3200.0f;
    constexpr float kDepth[5]   = { 3.82f, 11.91f, 20.57f, 32.09f, 150.0f };
    constexpr int   kFigLayer   = 1;
    constexpr float kDeckDepth  = 2.50f;
    constexpr float kDeckDy     = -2354.4f;
    constexpr float kDeckVhem   = -57.4f;
    constexpr float kDeckLift   = 5063.0f;
    constexpr float kLayerHem   = kDeckVhem - kDeckLift;    // -5120.4 units
    constexpr float kLayerExt   = 1200.0f;
    constexpr float kCameraY0   = (2160.0f - kLayerHem + 80.0f) * kDeckDepth / kFpx;
    constexpr float kParallaxP  = 0.2f;

    // select payoff (chp_*)
    constexpr float    kChalkEndMs    = 851.0f;
    constexpr float    kChalkCy       = 1140.0f;
    constexpr int      kChalkN        = 8;
    constexpr float    kChalkT0[kChalkN] = { 250.0f, 312.5f, 420.0f, 482.5f,
                                             590.0f, 647.5f, 745.0f, 787.5f };
    constexpr float    kChalkT1[kChalkN] = { 326.7f, 389.2f, 496.7f, 559.2f,
                                             663.3f, 720.8f, 808.3f, 850.8f };
    constexpr float    kPTextMs       = 250.0f;
    constexpr float    kPLiftMs       = 180.0f;
    constexpr float    kPSlamMs       = 70.0f;
    constexpr float    kPSettleMs     = 110.0f;
    constexpr float    kPLiftK        = 0.12f;
    constexpr float    kPSquashK      = 0.08f;
    constexpr float    kPRise         = 26.0f;
    constexpr uint64_t kPHoldMs       = 650;

    // intro cinematic (choose_intro_text.h + chi_* in choose.c)
    constexpr int   kIN      = 5;
    constexpr int   kBeatCount = 3;
    constexpr int   kLogoAt  = 22440;   // CHI_LOGO_IN_MS
    constexpr int   kTextOut = 26840;   // CHI_TEXT_OUT_MS
    constexpr int   kLastIn  = 19640;
    constexpr int   kFadeMs   = 1400;
    constexpr int   kBlackMs  = 1500;
    constexpr int   kOutMs2   = 1400;
    constexpr int   kPauseMs  = 350;
    constexpr int   kDropMs   = 6000;
    constexpr int   kUiLeadMs = 450;
    constexpr int   kUiInMs   = 1100;
    constexpr int   kInputMs  = 500;
    constexpr int   kSkipDownMs = 300;
    constexpr int   kSkipUpMs   = 550;

    // choose_intro_text.h: slot fade / beat fades and the logo line's layout.
    constexpr int   kIntroMax    = 6;
    constexpr int   kBeatFadeMs  = 600;
    constexpr int   kLogoFadeMs  = 2000;
    constexpr float kIntroPt     = 56.0f;
    constexpr float kSlotH       = 260.0f;
    constexpr float kLogoGap     = 22.0f;
    constexpr float kTailGap     = 4.0f;
    constexpr float kLogoSpan    = 362.3f;
    constexpr float kLogoDx      = -0.47f;
    constexpr float kLogoDy      = -3.0f;
    constexpr uint64_t kSelectSoundLeadMs = 50;   // the NR wave's hit lands on the slam

    struct Line {
        const char* text;
        int         cy;
        int         tin;
        bool        logo;
    };
    inline constexpr Line kLines[kIN] = {
        { "The Outbreak took the world twenty-five years ago.",  660,  1500, false },
        { "Seattle flooded. The survivors stayed.",              826,  5900, false },
        { "Supplies are running out. Every street is claimed.",  992,  9940, false },
        { "The Washington Liberation Front and\nthe followers of the Prophet fight for what's left.", 1206, 14340, false },
        { "Gather. Grow. Survive.",                             1420, 19640, true  },
    };
    inline constexpr const char* kBeats[kBeatCount] = { "Gather.", "Grow.", "Survive" };
    inline constexpr int kBeatAt[kBeatCount] = { 19640, 21040, 22440 };

    // ce_ease (clanend.c): the tween / alpha curve.
    inline float Ease(float t)
    {
        t = std::max(0.0f, std::min(1.0f, t));
        return t * t * (3.0f - 2.0f * t);
    }

    // side_ease (side_tick): the page crossfade and its entrance / exit ramps.
    inline float SideEase(float t)
    {
        t = std::max(0.0f, std::min(1.0f, t));
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    inline float ClampA(float a)
    {
        return std::max(0.0f, std::min(1.0f, a));
    }

    // ch_tw_t: an authored value ramp with a re-target that starts from the
    // value currently in flight (so a focus switch never snaps).
    struct Tween {
        float    from = 0.0f, to = 0.0f;
        uint64_t t0 = 0;
        float    ms = 1.0f;

        float Val(uint64_t now) const
        {
            if (ms <= 0.0f) return to;
            // A re-target before its start must hold the old value, never jump
            // to `to` (the ext's ch_tw_val clamps negative time to 0).
            if (now < t0) return from;
            const float u = static_cast<float>(now - t0) / ms;
            return from + (to - from) * Ease(std::min(1.0f, u));
        }
        void To(float target, uint64_t now, float duration)
        {
            if (to == target) return;
            from = Val(now);
            to = target;
            t0 = now;
            ms = duration > 0.0f ? duration : 1.0f;
        }
        void Reset(float value, uint64_t now)
        {
            from = to = value;
            t0 = now;
            ms = 1.0f;
        }
    };

    // The key-art picker's animation state (CH + CHA in choose.c).
    struct Art {
        bool     on = false, shown = false;
        int      focus = -1;               // -1 overview, 0 WLF, 1 Seraphites
        uint64_t foAt = 0;                 // when focus last changed
        uint64_t t0 = 0;
        float    twa = 1.0f;               // title-logo show amount
        uint64_t twt = 0;

        Tween zoom, pan;
        Tween name[2], logo[2], hl[2];     // hl: overview hover highlight
        int   hov = -1;                    // hovered camp (overview)
        bool  selHov = false;
        int   descFor = -1;
        uint64_t descAt = 0;

        void Reset(uint64_t now)
        {
            on = shown = false;
            focus = -1;
            foAt = t0 = twt = now;
            twa = 1.0f;
            zoom.Reset(0.0f, now);
            pan.Reset(-1.0f, now);
            for (int k = 0; k < 2; ++k) {
                name[k].Reset(0.0f, now);
                logo[k].Reset(1.0f, now);
                hl[k].Reset(0.0f, now);
            }
            hov = -1;
            selHov = false;
            descFor = -1;
            descAt = now;
        }
    };

    inline void Show(Art& a, bool enabled, uint64_t now)
    {
        a.Reset(now);
        a.on = enabled;
        a.shown = true;
    }

    // Returns true when the focus actually changed (the owner plays camp vox).
    inline bool Focus(Art& a, int f, uint64_t now)
    {
        if (!a.on || f < -1 || f > 1 || f == a.focus) return false;
        a.focus = f;
        a.foAt = now;
        a.zoom.To(f >= 0 ? 1.0f : 0.0f, now, kZoomMs);
        if (f >= 0) a.pan.To(f ? 1.0f : -1.0f, now, kPanMs);
        for (int k = 0; k < 2; ++k) {
            a.name[k].To(k == f ? 1.0f : 0.0f, now, kNameMs);
            a.logo[k].To(f < 0 ? 1.0f : (k == f ? kLogoUp : kLogoDown), now, kNameMs);
        }
        return true;
    }

    // Keys / pad: an overview focus goes left=WLF / right=Seraphites; a
    // focused camp toggles to the other one. Returns the new focus or -1 when
    // nothing changed.
    inline int Input(Art& a, bool left, bool right, uint64_t now)
    {
        if (!left && !right) return -1;
        const int target = a.focus < 0 ? (left ? 0 : 1) : (a.focus ^ 1);
        if (!Focus(a, target, now)) return -1;
        return target;
    }

    // Mouse: `zone` -2 = no live cursor, -1 = between camps, 0/1 a camp half;
    // `tab` is the logo/name under the cursor; `onSel` the SELECT hotspot.
    // Returns 1 when the click picks the focused camp, else 0.
    inline int Mouse(Art& a, int tab, int zone, bool onSel, bool click,
                     float mx, float my, bool confirmed, uint64_t now)
    {
        int hov;
        if (zone == -2) hov = -1;
        else if (a.focus < 0) {
            int col = (my >= kColY0 && my <= kColY1)
                ? (std::fabs(mx - kLogoCX[0]) < kColHW ? 0
                  : std::fabs(mx - kLogoCX[1]) < kColHW ? 1 : -1)
                : -1;
            hov = zone >= 0 ? zone : (col >= 0 ? col : (tab >= 0 && tab == a.hov ? tab : -1));
        } else hov = (tab >= 0 && tab != a.focus) ? tab : -1;

        if (hov != a.hov) {
            a.hov = hov;
            for (int k = 0; k < 2; ++k)
                a.hl[k].To(k == hov ? 1.0f : 0.0f, now, kHlMs);
        }
        if (a.focus < 0 && !confirmed)
            for (int k = 0; k < 2; ++k)
                a.name[k].To(k == hov ? 1.0f : 0.0f, now, kNameMs);
        a.selHov = a.focus >= 0 && onSel;
        if (!click || zone == -2) return 0;
        if (a.focus >= 0 && onSel) return 1;
        if (a.focus >= 0 && (zone == a.focus || tab == a.focus)
            && now - a.foAt > static_cast<uint64_t>(kNameMs)) return 1;
        if (a.focus < 0 && hov >= 0) { Focus(a, hov, now); return 0; }
        if (a.focus >= 0 && tab >= 0 && tab != a.focus) { Focus(a, tab, now); return 0; }
        return 0;
    }

    // Esc / B / right click: a focused camp goes back to the overview; the
    // page close belongs to the caller when there was no focus.
    inline bool Back(Art& a, bool confirmed, uint64_t now)
    {
        if (!a.on || a.focus < 0 || confirmed) return false;
        Focus(a, -1, now);
        return true;
    }

    inline float TitleAmount(Art& a, uint64_t now, int descTarget, bool introActive)
    {
        const float dt = a.twt ? static_cast<float>(now - a.twt) : 0.0f;
        a.twt = now;
        const bool free_ = !introActive && descTarget < 0 && a.focus < 0;
        a.twa = free_ ? std::min(1.0f, a.twa + dt / kFadeInMs)
                      : std::max(0.0f, a.twa - dt / kFadeOutMs);
        return a.twa;
    }

    // ---- select payoff (CHP) --------------------------------------------------

    struct Payoff {
        bool     on = false;
        int      side = -1;
        bool     snd = false;
        uint64_t t0 = 0;
    };

    inline void StartPayoff(Payoff& p, int side, bool artOn, uint64_t now)
    {
        p = Payoff{};
        if (!artOn) return;
        p.on = true;
        p.side = side;
        p.t0 = now;
    }

    inline bool PayoffDone(const Payoff& p, uint64_t now)
    {
        return !p.on || (now >= p.t0
            && now - p.t0 >= static_cast<uint64_t>(kChalkEndMs + kPHoldMs));
    }

    inline float PayoffTextAlpha(const Payoff& p, uint64_t now)
    {
        if (!p.on) return 1.0f;
        const int64_t el = static_cast<int64_t>(now) - static_cast<int64_t>(p.t0);
        return ClampA(1.0f - Ease(static_cast<float>(el) / kPTextMs));
    }

    struct LogoPose { float scale = 1.0f; float rise = 0.0f; };

    inline LogoPose PayoffLogo(const Payoff& p, uint64_t now)
    {
        LogoPose out;
        if (!p.on) return out;
        const float el = static_cast<float>(static_cast<int64_t>(now)
                                            - static_cast<int64_t>(p.t0));
        if (el < kPLiftMs) {
            const float u = el / kPLiftMs, e = 1.0f - (1.0f - u) * (1.0f - u);
            out.scale = 1.0f + kPLiftK * e;
            out.rise = kPRise * e;
        } else if (el < kPLiftMs + kPSlamMs) {
            const float u = (el - kPLiftMs) / kPSlamMs, e = u * u;
            out.scale = 1.0f + kPLiftK - (kPLiftK + kPSquashK) * e;
            out.rise = kPRise * (1.0f - e) - 4.0f * e;
        } else if (el < kPLiftMs + kPSlamMs + kPSettleMs) {
            const float e = Ease((el - kPLiftMs - kPSlamMs) / kPSettleMs);
            out.scale = 1.0f - kPSquashK * (1.0f - e);
            out.rise = -4.0f * (1.0f - e);
        }
        return out;
    }

    inline float ChalkAlpha(const Payoff& p, int k, uint64_t now)
    {
        if (!p.on || k < 0 || k >= kChalkN) return 0.0f;
        const float el = static_cast<float>(static_cast<int64_t>(now)
                                            - static_cast<int64_t>(p.t0));
        return ClampA((el - kChalkT0[k]) / (kChalkT1[k] - kChalkT0[k]));
    }

    // The sound hit is 50 ms in so it lands on the slam; the owner plays it
    // once and latches via `snd`.
    inline bool PayoffSoundDue(Payoff& p, uint64_t now)
    {
        if (!p.on || p.snd) return false;
        const float el = static_cast<float>(static_cast<int64_t>(now)
                                            - static_cast<int64_t>(p.t0));
        if (el < kPLiftMs + kPSlamMs - static_cast<float>(kSelectSoundLeadMs)) return false;
        p.snd = true;
        return true;
    }

    // ---- intro cinematic (CHI) ------------------------------------------------

    inline int IntroDropMs() { return kTextOut + kOutMs2 + kPauseMs; }
    inline int IntroUiMs()   { return IntroDropMs() + kDropMs - kUiLeadMs; }

    inline bool IntroInputOk(bool on, uint64_t now, uint64_t t0)
    {
        return !on || (now >= t0
                       && now - t0 >= static_cast<uint64_t>(IntroUiMs() + kInputMs));
    }

    // Height of the intro camera over the overview (metres). The tarp fills
    // the frame until CHI_T_DROP; then smootherstep(u^0.8) settles the 5.75 m
    // drop from rest. The owner feeds this into the layer / deck parallax.
    inline float IntroCameraYAt(int64_t el)
    {
        const int drop = IntroDropMs();
        if (el <= drop) return kCameraY0;
        if (el >= drop + kDropMs) return 0.0f;
        const float u = static_cast<float>(el - drop) / static_cast<float>(kDropMs);
        const float v = std::pow(std::max(0.0f, u), 0.8f);
        return kCameraY0 * (1.0f - SideEase(v));
    }

    inline float IntroCameraY(uint64_t now, uint64_t t0)
    {
        return IntroCameraYAt(now >= t0 ? static_cast<int64_t>(now - t0)
                                        : -static_cast<int64_t>(t0 - now));
    }

    inline float IntroLineAlpha(int64_t el, int k)
    {
        if (k < 0 || k >= kIN) return 0.0f;
        const float out = el < kTextOut ? 1.0f
            : ClampA(1.0f - Ease(static_cast<float>(el - kTextOut)
                                 / static_cast<float>(kOutMs2)));
        if (kLines[k].logo) return el >= kLines[k].tin ? out : 0.0f;
        return Ease(static_cast<float>(el - kLines[k].tin)
                    / static_cast<float>(kFadeMs)) * out;
    }

    inline float IntroBeatAlpha(int64_t el, int j)
    {
        if (j < 0 || j >= kBeatCount) return 0.0f;
        const float ms = static_cast<float>(kBeatAt[j] >= kLogoAt
                                            ? kLogoFadeMs : kBeatFadeMs);
        return Ease(static_cast<float>(el - kBeatAt[j]) / ms);
    }

    inline float IntroLogoAlpha(int64_t el)
    {
        return Ease(static_cast<float>(el - kLogoAt)
                    / static_cast<float>(kLogoFadeMs));
    }

    inline float IntroSkipAlpha(int64_t el, bool skippable)
    {
        if (!skippable || el >= IntroDropMs()) return 0.0f;
        return Ease(static_cast<float>(el - 800.0f) / 400.0f)
             * (1.0f - Ease(static_cast<float>(el - (IntroDropMs() - 300.0f))
                            / 300.0f));
    }

    inline float IntroUiAlpha(int64_t el)
    {
        if (el < IntroUiMs()) return 0.0f;
        return Ease(static_cast<float>(el - IntroUiMs())
                    / static_cast<float>(kUiInMs));
    }

    inline float SkipDownAlpha(int64_t el) { return Ease(static_cast<float>(el) / kSkipDownMs); }
    inline float SkipUpAlpha(int64_t el)   { return 1.0f - Ease(static_cast<float>(el) / kSkipUpMs); }

    struct LayerPose { float scale = 1.0f, x = 1920.0f, y = 1080.0f; };

    // choose.c ch_tick's key-art camera: every layer has its own depth, so a
    // dolly / truck moves each at its own rate. The figure layer holds the
    // framing; the others are expressed relative to it, then clamped so a
    // 5120 x 2160 layer always covers the 3840 x 2160 screen.
    inline LayerPose LayerAt(int i, float zoomT, float panT, float seconds,
                             float cameraY)
    {
        LayerPose out;
        if (i < 0 || i >= 5) return out;
        const float t = seconds;
        const float X = kTruck * panT * zoomT
                      + 0.10f * std::sin(t * 0.23f) + 0.04f * std::sin(t * 0.61f);
        const float A = kDolly * zoomT + 0.10f * std::sin(t * 0.17f);
        const float Y = 0.035f * std::sin(t * 0.19f);
        const float Z = 1.0f + zoomT * (kZoom - 1.0f);
        const float Gx = 1920.0f + 1920.0f * panT;
        float sg[5], tx[5], ty[5];
        for (int k = 0; k < 5; ++k) {
            const float dd = std::max(0.6f, kDepth[k] - A);
            sg[k] = kDepth[k] / dd;
            tx[k] = -kFpx * X / dd;
            ty[k] = kFpx * Y / dd;
        }
        const float S = Z * sg[i] / sg[kFigLayer];
        float px = 1920.0f + (tx[i] - tx[kFigLayer]) / sg[kFigLayer];
        float py = 1080.0f + (ty[i] - ty[kFigLayer]) / sg[kFigLayer];
        px = Gx + Z * (px - Gx);
        py = kGy + Z * (py - kGy);
        float lo = 3840.0f - 2560.0f * S, hi = 2560.0f * S;
        if (lo > hi) lo = hi = 0.5f * (lo + hi);
        px = std::max(lo, std::min(hi, px));
        float vlo = 2160.0f - 1080.0f * S, vhi = 1080.0f * S;
        if (vlo > vhi) vlo = vhi = 0.5f * (vlo + vhi);
        py = std::max(vlo, std::min(vhi, py));
        if (cameraY > 0.0f) {
            const float off = kFpx * cameraY / kDepth[1]
                * std::pow(kDepth[1] / kDepth[i], kParallaxP);
            const float hf = kLayerHem + kFpx * cameraY / kDeckDepth;
            const float lim = std::max(0.0f, hf) + kLayerExt * S;
            py += std::min(off, lim);
        }
        out.scale = S;
        out.x = px;
        out.y = py;
        return out;
    }

    inline LayerPose DeckAt(float zoomT, float panT, float seconds, float cameraY)
    {
        LayerPose out;
        const float t = seconds;
        const float X = kTruck * panT * zoomT
                      + 0.10f * std::sin(t * 0.23f) + 0.04f * std::sin(t * 0.61f);
        const float A = kDolly * zoomT + 0.10f * std::sin(t * 0.17f);
        const float Y = 0.035f * std::sin(t * 0.19f);
        const float Z = 1.0f + zoomT * (kZoom - 1.0f);
        const float Gx = 1920.0f + 1920.0f * panT;
        float sg[5], tx[5], ty[5];
        for (int k = 0; k < 5; ++k) {
            const float dd = std::max(0.6f, kDepth[k] - A);
            sg[k] = kDepth[k] / dd;
            tx[k] = -kFpx * X / dd;
            ty[k] = kFpx * Y / dd;
        }
        const float dd = std::max(0.3f, kDeckDepth - A);
        const float sgd = kDeckDepth / dd;
        const float S = Z * sgd / sg[kFigLayer];
        float px = 1920.0f + (-kFpx * X / dd - tx[kFigLayer]) / sg[kFigLayer];
        float py = 1080.0f + (kFpx * Y / dd - ty[kFigLayer]) / sg[kFigLayer];
        px = Gx + Z * (px - Gx);
        py = kGy + Z * (py - kGy);
        out.scale = S;
        out.x = px;
        out.y = py + (kDeckDy - kDeckLift) * S + kFpx * cameraY / kDeckDepth;
        return out;
    }

    // ---- the side_tick page stage machine ------------------------------------

    struct Page {
        int      stage = kIntro;
        int      focus = 0;
        int      fadeMs = 220;             // 220 from the intro card, 800 from end screens
        uint64_t enterAt = 0, exitAt = 0, fadeAt = 0;
        int      pickSide = -1;            // confirmed side (0/1), -1 = back
        bool     pickStarted = false;
        bool     pickDemo = false;
        bool     ceTest = false;
        bool     living = false;              // side_tick's c->st == 1 (back closes the page)
        uint64_t pickAt = 0;
        uint64_t backFrameAt = 0;
        uint64_t lastInputAt = 0;
        bool     wasFocused = false;
    };

    inline void Enter(Page& p, bool endScreens, bool artAndIntro,
                      bool rc, uint64_t now)
    {
        p = Page{};
        p.stage = endScreens ? kEndScreen : (artAndIntro ? kPick : kIntro);
        p.fadeMs = endScreens ? 800 : 220;
        p.enterAt = now;
        (void)rc;
    }

    // side_tick stage 0: SELECT goes to the crossfade into the pick.
    inline bool IntroSelect(Page& p, uint64_t now)
    {
        if (p.stage != kIntro) return false;
        p.stage = kCrossfade;
        p.fadeAt = now;
        p.enterAt = now - static_cast<uint64_t>(kInMs + kStaggerMs + 100);
        p.fadeMs = 220;
        return true;
    }

    // side_tick stage 2: returns true when the crossfade completes.
    inline bool CrossfadeDone(Page& p, uint64_t now)
    {
        if (p.stage != kCrossfade) return false;
        const float t = static_cast<float>(now - p.fadeAt) / static_cast<float>(p.fadeMs);
        if (t < 1.0f) return false;
        p.stage = kPick;
        return true;
    }

    inline float CrossfadeAlpha(const Page& p, uint64_t now)
    {
        const float t = ClampA(static_cast<float>(now - p.fadeAt)
                               / static_cast<float>(p.fadeMs));
        return p.fadeMs > 220 ? SideEase(t) : t;
    }

    inline void Confirm(Page& p, int side, bool art, bool demo, uint64_t now)
    {
        p.pickSide = side;
        p.pickDemo = demo;
        if (art) {
            p.stage = kPayoff;
        } else {
            p.stage = kFadeOut;
            p.exitAt = now;
            p.pickStarted = false;
        }
    }

    inline void PayoffEnded(Page& p, uint64_t now)
    {
        p.stage = kFadeOut;
        p.exitAt = now;
        p.pickStarted = false;
    }

    inline bool FadeOutDone(const Page& p, uint64_t now)
    {
        if (p.stage != kFadeOut) return false;
        return now >= p.exitAt
            && now - p.exitAt >= (p.pickSide >= 0 ? kPickOutMs : kOutMs);
    }

    inline uint64_t OutMs(const Page& p)
    {
        return p.pickSide >= 0 ? kPickOutMs : kOutMs;
    }

    // Page ramps: the backdrop eases to SIDE_BG_A over SIDE_IN_MS, the content
    // SIDE_STAGGER_MS later; the exit uses the same smootherstep.
    inline float BackgroundIn(const Page& p, uint64_t now)
    {
        if (p.stage == kFadeOut && p.pickSide >= 0) return 1.0f;
        const float t = now >= p.enterAt ? static_cast<float>(now - p.enterAt)
                                         / static_cast<float>(kInMs) : 0.0f;
        return kBgA * SideEase(t);
    }

    inline float ContentIn(const Page& p, uint64_t now)
    {
        const int64_t el = now >= p.enterAt ? static_cast<int64_t>(now - p.enterAt) : 0;
        return SideEase(static_cast<float>(el - static_cast<int64_t>(kStaggerMs))
                        / static_cast<float>(kInMs));
    }

    inline float ExitAmount(const Page& p, uint64_t now)
    {
        if (p.stage != kFadeOut) return 1.0f;
        const float t = now >= p.exitAt ? static_cast<float>(now - p.exitAt)
                                        / static_cast<float>(OutMs(p)) : 0.0f;
        return 1.0f - SideEase(t);
    }

    inline float BackgroundOut(const Page& p, uint64_t now)
    {
        if (p.stage != kFadeOut) return kBgA;
        const float eo = ExitAmount(p, now);
        return p.pickSide >= 0 ? kBgA + (1.0f - kBgA) * (1.0f - eo)
                               : kBgA * eo;
    }

    inline void Popped(Page& p)
    {
        p.stage = kPopped;
        p.pickSide = -1;
        p.pickDemo = false;
        p.pickStarted = false;
    }

    // Back: a focused camp goes to the overview; otherwise the page closes.
    inline bool BackOrClose(Page& p, bool artBackUsed, uint64_t frame, uint64_t now)
    {
        if (artBackUsed) return true;
        // A clan with no side has nothing to go back to; only the demo / a
        // living clan closes the page (side_tick's back_at latch).
        if (!p.backFrameAt && (p.ceTest || p.living)) p.backFrameAt = frame;
        if (p.backFrameAt && frame - p.backFrameAt > 6) {
            p.backFrameAt = 0;
            Popped(p);
            return true;
        }
        (void)now;
        return false;
    }

    // The one-time re-choice warning note is only written while the pick is
    // up and the pick has not been confirmed (side_rc_note in side_tick).
    inline bool ShowRechoiceNote(const Page& p)
    {
        return (p.stage == kPick || p.stage == kCrossfade) && p.pickSide < 0;
    }

} // namespace net::ui::choose::core
