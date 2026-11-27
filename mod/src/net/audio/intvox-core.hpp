#pragma once

// Pure interrogation-voice rules (tools/ext/intvox.c), with no engine, audio,
// network, sidecar or private-data headers and no clock of its own. The native
// owner in intvox.cpp supplies the paired move, the move clock, the sidecar
// answers, the DC replies, the private BEG_INT table and the voice mapping.
// Kept exactly where the ext keeps it:
//   * the fist timeline (question -> victim answer -> slam -> shout -> desperate
//     answer -> kill) and the two-line shiv timeline, in REAL ms of the move;
//   * deterministic PSYNC-seeded picks with a per-match text history, a used-
//     source guard, the once-per-interrogation "Fuck you!" rule and the
//     duration caps that keep a reply before the slam / swing;
//   * the 100 ms same-speaker gap, the victim's cut at the kill frame, the
//     140 ms is-speaking? refusal check and the effort-event fallback;
//   * the intwarp clip -> real mapping (intwarp.c), used only when a producer
//     reports that the native animation is actually slowed.
// Tests: mod/tests/intvox_core_test.cpp.

#include <cstdint>

namespace net::audio::intvox::core {

    inline constexpr int kMaxSteps  = 12;    // ext IVX_MAX
    inline constexpr int kMaxUsed   = 8;     // ext g_ivx.used[8]
    inline constexpr int kHistoryN  = 6;     // ext IVH_N
    inline constexpr int kCheckMs   = 140;   // ext EB_CHECK_MS
    inline constexpr int kMinKind   = 8;     // first BEG_INT kind in the table
    inline constexpr int kMaxKind   = 14;    // last BEG_INT kind in the table
    inline constexpr int kKindCount = kMaxKind - kMinKind + 1;

    // pseudo-kinds the timeline uses for non-dialogue beats: 20 interrogator
    // slam grunt, 21 victim slam / death take, 22 head slam (rec -2 = final).
    inline constexpr int kSlamGive = 20;
    inline constexpr int kSlamTake = 21;
    inline constexpr int kSlamHit  = 22;

    // ---- intwarp.c (the fist interrogation's slowed hold) -------------------
    // A pure, host-independent integral of 1 / rate over the authored clip.
    // The native tree does not slow the animation, so the owner leaves `warped`
    // false by default (the move clock is already real ms). A future native
    // intwarp producer can set it true and get the ext's 7.98 s kill moment.
    inline constexpr int kClipMs = 7000;     // ext IW_N
    inline constexpr double kWarpHoldCenter = 1500.0;
    inline constexpr double kWarpHoldLen    = 300.0;

    namespace detail {

        inline double Smooth(double u) noexcept
        {
            u = u < 0.0 ? 0.0 : u > 1.0 ? 1.0 : u;
            return u * u * (3.0 - 2.0 * u);
        }

        inline double Energy(double clipMs) noexcept
        {
            // ext IW_E: summed joint motion (attacker + victim) per 100 ms,
            // 3-tap smoothed; endpoints hold for clips outside the table.
            static constexpr float kE[] = {
                1.285f, 1.818f, 2.774f, 3.089f, 2.889f, 2.744f, 2.65f, 2.424f, 2.22f, 2.277f,
                2.302f, 2.325f, 2.676f, 2.805f, 2.533f, 2.33f, 2.025f, 1.821f, 1.845f, 2.191f,
                2.877f, 2.754f, 1.86f, 1.445f, 1.382f, 1.255f, 1.317f, 1.418f, 1.336f, 1.203f,
                1.053f, 0.906f, 0.848f, 0.815f, 0.745f, 0.705f, 0.827f, 1.56f, 2.375f, 2.332f,
                1.873f, 1.565f, 1.582f, 1.611f, 1.426f, 1.348f, 1.607f, 2.257f, 2.929f, 2.922f
            };
            const double n = static_cast<double>(sizeof kE / sizeof kE[0]);
            const double f = (clipMs - 50.0) / 100.0;
            if (f <= 0.0)
                return kE[0];
            if (f >= n - 1.0)
                return kE[static_cast<int>(n) - 1];
            const int i = static_cast<int>(f);
            return static_cast<double>(kE[i])
                 + (static_cast<double>(kE[i + 1]) - static_cast<double>(kE[i])) * (f - i);
        }

        inline double Rate(double c) noexcept
        {
            double r = 1.0;
            // ext IW_SEC: two eased sections, each with its own low floor.
            for (int k = 0; k < 2; ++k) {
                const double a = k == 0 ? 350.0 : 2400.0;
                const double b = k == 0 ? 1650.0 : 4250.0;
                if (c <= a || c >= b)
                    continue;
                double w = Smooth((c - a) / 250.0);
                const double w2 = Smooth((b - c) / 250.0);
                if (w2 < w)
                    w = w2;
                const double e = Energy(c);
                const double movFloor = 0.72 - (0.72 - 0.60) * Smooth((1.6 - e) / 0.6);
                const double rr = movFloor + (1.0 - movFloor) * Smooth((e - 2.0) / (3.6 - 2.0));
                const double section = 1.0 - (1.0 - rr) * w;
                if (section < r)
                    r = section;
            }
            // the extra settled-mount hold (clip 1350..1650 ms).
            const double h0 = kWarpHoldCenter - kWarpHoldLen * 0.5;
            if (c > h0 && c < h0 + kWarpHoldLen) {
                const double w1 = Smooth((c - h0) / 120.0);
                const double w2 = Smooth((h0 + kWarpHoldLen - c) / 120.0);
                const double w = w1 < w2 ? w1 : w2;
                const double rh = 1.0 - (1.0 - 0.20) * w;
                if (rh < r)
                    r = rh;
            }
            return r;
        }

        inline const double* WarpTable() noexcept
        {
            static double tab[kClipMs + 1] = {};
            static bool built = false;
            if (!built) {
                tab[0] = 0.0;
                for (int i = 1; i <= kClipMs; ++i)
                    tab[i] = tab[i - 1] + 1.0 / Rate(static_cast<double>(i) - 0.5);
                built = true;
            }
            return tab;
        }

    } // namespace detail

    // ext iw_real_ms: authored clip ms -> real ms of the move.
    inline double ClipToRealMs(double clipMs) noexcept
    {
        if (clipMs <= 0.0)
            return clipMs;
        const double* t = detail::WarpTable();
        if (clipMs >= static_cast<double>(kClipMs))
            return t[kClipMs] + (clipMs - static_cast<double>(kClipMs));
        const int i = static_cast<int>(clipMs);
        const double f = clipMs - i;
        return t[i] + (t[i + 1] - t[i]) * f;
    }

    // ext IW_EXTRA: how much later the kill lands in real time.
    inline double WarpExtraMs() noexcept
    {
        return detail::WarpTable()[kClipMs] - static_cast<double>(kClipMs);
    }

    // ---- private BEG_INT data (see intvox-data.hpp) -------------------------
    // Only kinds 8..14 are stored: 12 / 15 / 16 have no authored takes in the
    // in-tree generator, and kind 16 is cut by the ext itself.
    struct VoxTable {
        const uint64_t* lines = nullptr;   // VXG_LINES
        const uint16_t* range = nullptr;   // VXG_RANGE [fac][female][voice][kind 8..14][2]
        const uint16_t* dur   = nullptr;   // VXG_DUR (voiced length, ms)
        const uint8_t*  flag  = nullptr;   // VXG_FLAG, bit 0 = "Fuck you!"
        const uint32_t* src   = nullptr;   // VXG_SRC (source take identity)
        const uint32_t* txt   = nullptr;   // VXG_TXT (subtitle identity for history)
        const uint8_t*  avail = nullptr;   // active-language stream filter, nullptr = all
        int             count = 0;         // VXG_N_INT

        int Range(int fac, int female, int voice, int kind, int field) const noexcept
        {
            if (!range || fac < 0 || fac > 1 || female < 0 || female > 1
                || voice < 0 || voice > 3 || kind < kMinKind || kind > kMaxKind
                || field < 0 || field > 1)
                return 0;
            const int i = ((((fac * 2 + female) * 4 + voice) * kKindCount + (kind - kMinKind)) * 2) + field;
            return range[i];
        }
        uint64_t Line(int rec) const noexcept { return lines && rec >= 0 && rec < count ? lines[rec] : 0; }
        int Dur(int rec) const noexcept { return dur && rec >= 0 && rec < count ? dur[rec] : 0; }
        bool Flag(int rec) const noexcept { return flag && rec >= 0 && rec < count && (flag[rec] & 1) != 0; }
        uint32_t Src(int rec) const noexcept { return src && rec >= 0 && rec < count ? src[rec] : 0; }
        uint32_t Txt(int rec) const noexcept { return txt && rec >= 0 && rec < count ? txt[rec] : 0; }
        bool Avail(int rec) const noexcept { return !avail || (rec >= 0 && rec < count && avail[rec] != 0); }
    };

    // ---- state --------------------------------------------------------------

    struct Step {
        double   at   = 0.0;
        uint8_t  who  = 0;     // 0 interrogator, 1 victim
        uint8_t  kind = 0;
        int      rec  = -1;    // -1 pick now, -2 final smash
    };

    struct State {
        double   t0       = 0.0;      // paired-move clock at the start
        int      on       = 0;        // 0 idle, 1 running, 2 victim cut at the kill
        int      step     = 0;
        int      nstep    = 0;
        int      fac[2]   = { -1, -1 };
        int      female[2]= { -1, -1 };
        int      voice[2] = { -1, -1 };
        bool     ok[2]    = { false, false };
        int      last[2]  = { -1, -1 };
        Step     st[kMaxSteps];
        double   busy[2]  = { 0.0, 0.0 };
        double   vline[2] = { 0.0, 0.0 };   // victim line window for intfoley
        double   cutAt[2] = { 0.0, 0.0 };
        double   killMs   = 0.0;
        uint32_t seed     = 0;
        bool     fuck     = false;
        int      nused    = 0;
        uint32_t used[kMaxUsed] = {};
        double   shift    = 0.0;      // native move clock is real, so normally 0
        bool     spoke[2] = { false, false };
        int      capMs    = 0;
        bool     shiv     = false;
        uint64_t ent[2]   = { 0, 0 };
        bool     refused  = false;    // dialogue refused THIS interrogation
        uint64_t checkAt  = 0;        // wall clock of the 140 ms speaking check
        int      checkW   = 0;
        int      checkKind= 0;
        bool     armed    = false;
        int      tries    = 0;
        // the match-long no-repeat history (survives Begin, not Reset)
        uint32_t hist[3][kHistoryN] = {};
        int      histN[3] = { 0, 0, 0 };
    };

    inline void ResetHistory(State& s) noexcept
    {
        for (int k = 0; k < 3; ++k) {
            for (int i = 0; i < kHistoryN; ++i)
                s.hist[k][i] = 0;
            s.histN[k] = 0;
        }
    }

    inline void Reset(State& s) noexcept
    {
        s = State{};
    }

    inline int HistSlot(int kind) noexcept
    {
        return kind == 9 ? 0 : kind == 11 ? 1 : kind == 14 ? 2 : -1;
    }

    inline bool HistHas(const State& s, int hk, uint32_t text) noexcept
    {
        if (hk < 0 || hk > 2)
            return false;
        const int n = s.histN[hk] < kHistoryN ? s.histN[hk] : kHistoryN;
        for (int i = 0; i < n; ++i)
            if (s.hist[hk][i] == text)
                return true;
        return false;
    }

    inline uint32_t PickHash(uint32_t seed, int kind, uint32_t salt) noexcept
    {
        uint32_t hs = (seed ^ (static_cast<uint32_t>(kind) * 0x9e3779b1u)
                            ^ (salt * 0x85ebca6bu)) * 2654435761u;
        hs ^= hs >> 15;
        hs *= 2246822519u;
        hs ^= hs >> 13;
        return hs;
    }

    inline double StepDur(const VoxTable& t, int rec) noexcept
    {
        if (rec < 0)
            return 600.0;
        const int d = t.Dur(rec);
        return d > 0 ? static_cast<double>(d) : 900.0;
    }

    // ext ivx_pick: two passes (the second only for history kinds), duration
    // cap, "Fuck you!" once, no source twice, no subtitle twice in the last six
    // interrogations, and no immediate repeat for the same speaker.
    inline int Pick(State& s, const VoxTable& t, int w, int kind, uint32_t salt) noexcept
    {
        if (kind == 16 || w < 0 || w > 1 || !s.ok[w])
            return -1;
        const uint32_t hs = PickHash(s.seed, kind, salt);
        const int hk = HistSlot(kind);
        int cand[32];
        int nc = 0, nhist = 0;
        const int first = t.Range(s.fac[w], s.female[w], s.voice[w], kind, 0);
        const int n = t.Range(s.fac[w], s.female[w], s.voice[w], kind, 1);
        for (int pass = 0; pass < 2 && !nc; ++pass) {
            for (int i = 0; i < n && nc < 32; ++i) {
                const int r = first + i;
                if (r < 0 || r >= t.count)
                    continue;
                if (!t.Avail(r))
                    continue;
                if (s.capMs > 0 && t.Dur(r) > s.capMs)
                    continue;
                if (t.Flag(r) && s.fuck)
                    continue;
                bool dup = false;
                for (int u = 0; u < s.nused; ++u)
                    if (s.used[u] == t.Src(r))
                        dup = true;
                if (!pass && hk >= 0 && HistHas(s, hk, t.Txt(r))) {
                    ++nhist;
                    continue;
                }
                if (!dup)
                    cand[nc++] = r;
            }
            if (hk < 0)
                break;
        }
        (void)nhist;
        if (!nc)
            return -1;
        int ci = static_cast<int>(hs % static_cast<uint32_t>(nc));
        if (nc > 1 && cand[ci] == s.last[w])
            ci = (ci + 1) % nc;
        const int r = cand[ci];
        if (t.Flag(r))
            s.fuck = true;
        if (s.nused < kMaxUsed)
            s.used[s.nused++] = t.Src(r);
        if (hk >= 0) {
            s.hist[hk][s.histN[hk] % kHistoryN] = t.Txt(r);
            ++s.histN[hk];
        }
        return r;
    }

    // ext ivx_unpick / ivx_repick: take a pick back so the beat can be picked
    // again under a tighter cap. The history entry must be the newest of its kind.
    inline void Unpick(State& s, const VoxTable& t, int rec) noexcept
    {
        if (rec < 0)
            return;
        for (int u = s.nused - 1; u >= 0; --u) {
            if (s.used[u] != t.Src(rec))
                continue;
            for (int i = u + 1; i < s.nused; ++i)
                s.used[i - 1] = s.used[i];
            --s.nused;
            break;
        }
        if (t.Flag(rec))
            s.fuck = false;
        for (int hk = 0; hk < 3; ++hk) {
            if (s.histN[hk] <= 0)
                continue;
            if (s.hist[hk][(s.histN[hk] - 1) % kHistoryN] == t.Txt(rec)) {
                --s.histN[hk];
                break;
            }
        }
    }

    inline void Repick(State& s, const VoxTable& t, int rec) noexcept
    {
        if (rec < 0)
            return;
        if (t.Flag(rec))
            s.fuck = true;
        if (s.nused < kMaxUsed)
            s.used[s.nused++] = t.Src(rec);
        const int hk = 0;                 // ext repicks the kind-9 slot
        s.hist[hk][s.histN[hk] % kHistoryN] = t.Txt(rec);
        ++s.histN[hk];
    }

    inline int PickFit(State& s, const VoxTable& t, int w, int kind,
                       uint32_t salt, int capMs) noexcept
    {
        if (capMs < 300)
            return -1;
        s.capMs = capMs;
        const int r = Pick(s, t, w, kind, salt);
        s.capMs = 0;
        return r;
    }

    inline int PickCapped(State& s, const VoxTable& t, int w, int kind,
                          uint32_t salt, int capMs) noexcept
    {
        s.capMs = capMs;
        const int r = Pick(s, t, w, kind, salt);
        s.capMs = 0;
        return r >= 0 ? r : Pick(s, t, w, kind, salt);
    }

    // ---- timeline -----------------------------------------------------------

    struct BeginIn {
        double   t0       = 0.0;      // move clock at the start
        double   killMs   = 4600.0;   // real ms of the kill frame
        double   impactMs = 2267.0;   // slam impact, clip ms
        double   swingMs  = 4300.0;   // kill swing, clip ms
        double   smashMs  = 5000.0;   // final head smash, clip ms
        bool     warped   = false;    // caller maps clip beats when the anim is slowed
        bool     fist     = true;
        uint32_t uid      = 0;
        int      pair     = -1;
        int      attSlot  = -1, vicSlot = -1;
        uint64_t ent[2]   = { 0, 0 };
        int      fac[2]   = { -1, -1 };
        int      female[2]= { -1, -1 };
        int      voice[2] = { -1, -1 };
        bool     ok[2]    = { false, false };
    };

    inline uint32_t MoveSeed(const BeginIn& in) noexcept
    {
        const uint32_t sa = static_cast<uint32_t>(in.attSlot + 1);
        const uint32_t sv = static_cast<uint32_t>(in.vicSlot + 7);
        const uint32_t uid = in.uid ? in.uid : static_cast<uint32_t>(in.t0);
        return (uid * 0x2545f491u)
             ^ (static_cast<uint32_t>(in.pair + 1) * 0x9e3779b1u)
             ^ (sa * 0x85ebca6bu) ^ (sv * 0xc2b2ae35u);
    }

    // ext intvox_tick's first-frame timeline construction. `Begin` clears the
    // per-run state but leaves the match history and the used-source wheel.
    inline void Begin(State& s, const VoxTable& t, const BeginIn& in) noexcept
    {
        // A new interrogation clears the run (ext's memset zeroes g_ivx: the
        // used-source wheel and the "Fuck you!" latch are per interrogation)
        // but keeps the match-long no-repeat history in g_ivh.
        uint32_t hist[3][kHistoryN];
        int histN[3];
        for (int k = 0; k < 3; ++k) {
            histN[k] = s.histN[k];
            for (int i = 0; i < kHistoryN; ++i)
                hist[k][i] = s.hist[k][i];
        }
        s = State{};
        for (int k = 0; k < 3; ++k) {
            s.histN[k] = histN[k];
            for (int i = 0; i < kHistoryN; ++i)
                s.hist[k][i] = hist[k][i];
        }
        s.t0 = in.t0;
        s.on = 1;
        s.last[0] = s.last[1] = -1;
        s.shiv = !in.fist;
        s.ent[0] = in.ent[0];
        s.ent[1] = in.ent[1];
        s.killMs = in.killMs + (in.fist && in.warped ? WarpExtraMs() : 0.0);
        for (int w = 0; w < 2; ++w) {
            s.fac[w] = in.fac[w];
            s.female[w] = in.female[w];
            s.voice[w] = in.voice[w];
            s.ok[w] = in.ok[w];
        }
        s.seed = MoveSeed(in);

        const double impact = in.warped ? ClipToRealMs(in.impactMs) : in.impactMs;
        const double swing  = in.warped ? ClipToRealMs(in.swingMs)  : in.swingMs;
        const double smash  = in.warped ? ClipToRealMs(in.smashMs)  : in.smashMs;
        const double pause = 600.0, gap = 300.0;
        int n = 0;
        auto add = [&](double at, int who, int kind, int rec) {
            if (n < kMaxSteps) {
                s.st[n].at = at;
                s.st[n].who = static_cast<uint8_t>(who);
                s.st[n].kind = static_cast<uint8_t>(kind);
                s.st[n].rec = rec;
                ++n;
            }
        };

        if (in.fist) {
            int r9 = s.ok[1] ? PickCapped(s, t, 1, 9, 9, 1350) : -1;
            double d9 = r9 >= 0 ? StepDur(t, r9) : 0.0;
            int q1cap = static_cast<int>(impact - 120.0 - pause - d9 - 450.0);
            if (q1cap < 700)
                q1cap = 700;
            int r8 = s.ok[0] ? PickCapped(s, t, 0, 8, 8, q1cap) : -1;
            const double t8 = 450.0, e8 = t8 + StepDur(t, r8);
            double t9 = e8 + pause;
            if (r9 >= 0 && e8 + 250.0 + d9 > impact + 150.0) {
                const int cap = static_cast<int>(impact + 150.0 - (e8 + 250.0));
                Unpick(s, t, r9);
                const int f9 = PickFit(s, t, 1, 9, 19, cap);
                if (f9 >= 0) {
                    r9 = f9;
                    d9 = StepDur(t, r9);
                } else {
                    Repick(s, t, r9);
                }
            }
            if (t9 + d9 > impact - 120.0) {
                t9 = e8 + 250.0;
                if (t9 + d9 > impact + 150.0)
                    t9 = impact + 150.0 - d9;
                if (t9 < e8 + 100.0)
                    t9 = e8 + 100.0;
            }
            const double e9 = r9 >= 0 ? t9 + d9 : e8;
            const int r10 = s.ok[0] ? Pick(s, t, 0, 10, 10) : -1;
            const double t10 = impact + 500.0 > e9 + 300.0 ? impact + 500.0 : e9 + 300.0;
            const double e10 = t10 + StepDur(t, r10);
            const double t11 = e10 + gap;
            const int r11 = s.ok[1] ? PickFit(s, t, 1, 11, 11,
                                               static_cast<int>(swing + 400.0 - t11)) : -1;
            add(t8, 0, 8, r8);
            add(in.warped ? ClipToRealMs(2150.0) : 2150.0, 0, kSlamGive, -1);
            add(in.warped ? ClipToRealMs(2200.0) : 2200.0, 1, kSlamTake, -1);
            add(impact, 1, kSlamHit, -1);
            add(smash - 60.0, 0, kSlamGive, -1);
            add(smash, 1, kSlamHit, -2);
            if (r9 >= 0) add(t9, 1, 9, r9);
            if (r10 >= 0) add(t10, 0, 10, r10);
            if (r11 >= 0) add(t11, 1, 11, r11);
            add(s.killMs + 20.0, 1, kSlamTake, -1);
        } else {
            const int r13 = s.ok[0] ? PickCapped(s, t, 0, 13, 13, 1500) : -1;
            const int r14 = s.ok[1] ? PickCapped(s, t, 1, 14, 14, 1500) : -1;
            const double e13 = 350.0 + StepDur(t, r13);
            const double t14 = e13 + 450.0 > 700.0 ? e13 + 450.0 : 700.0;
            add(350.0, 0, 13, r13);
            add(t14, 1, 14, r14);
        }
        for (int a = 1; a < n; ++a) {          // ext's insertion sort by time
            for (int b = a; b > 0 && s.st[b - 1].at > s.st[b].at; --b) {
                const Step x = s.st[b];
                s.st[b] = s.st[b - 1];
                s.st[b - 1] = x;
            }
        }
        s.nstep = n;
    }

    // ---- cue output ---------------------------------------------------------

    enum class CueKind : int {
        kNone = 0, kSayRecord, kSayEffort, kSlamGive, kSlamTake, kSlamHit,
        kStopVictim, kClearLook
    };

    struct Cue {
        CueKind kind = CueKind::kNone;
        int     who  = 0;
        int     rec  = -1;
        int     kindNo = 0;      // authored kind or pseudo-kind of the step
        int     priority = 2;    // ext `say` priority (26 on the one busy retry)
        double  dur  = 0.0;
        double  ems  = 0.0;
        bool    second = false;  // final smash
    };

    struct LineStep { double at = 0.0; };
    struct LineFeed {
        bool      on = false;
        double    t0 = 0.0;
        double    shift = 0.0;
        double    busy[2] = { 0.0, 0.0 };
        double    vline[2] = { 0.0, 0.0 };
        int       step = 0, count = 0;
        LineStep  steps[8];
    };

    struct Result {
        Cue cues[kMaxSteps + 2];
        int  count = 0;
        LineFeed line;
        bool lineClear = false;
        bool quietUntilSet = false;
        uint64_t quietUntil = 0;
        bool look = false;
        bool lookClear = false;
        bool end = false;
        const char* why = nullptr;

        void Add(const Cue& c) noexcept
        {
            if (count < static_cast<int>(sizeof cues / sizeof cues[0]))
                cues[count++] = c;
        }
    };

    inline bool EarlyKind(int kind) noexcept
    {
        return kind == 8 || kind == 9 || kind == 10 || kind == 11
            || kind == 13 || kind == 14;
    }

    // ext ivx_say's fallback list: the moment's generic effort event when the
    // voice set has no verbal take, or when `say` refused the record.
    inline const char* EffortEvent(int w, int kind, int slot) noexcept
    {
        static constexpr const char* const kGive[] = {
            "vox-eff-gen-fight-give-lrg-short", "vox-eff-gen-fight-give-hvy-short", nullptr
        };
        static constexpr const char* const kTake[] = {
            "vox-eff-gen-begs-for-life-stun", "vox-eff-gen-fight-take-hvy", nullptr
        };
        static constexpr const char* const kHit[] = {
            "vox-eff-gen-fight-take-hvy", "vox-eff-gen-fight-take-lrg",
            "vox-eff-gen-fight-take-med", nullptr
        };
        const char* const* row = kind >= 20 ? (w == 0 ? kGive : kHit) : (w == 0 ? kGive : kTake);
        if (slot < 0 || slot > 3)
            return nullptr;
        return row[slot];
    }

    inline void Speak(State& s, Result& out, const VoxTable& t, int w, int kind,
                      int rec, double ems, uint64_t now) noexcept
    {
        if (rec < 0 && !s.refused && s.ok[w])
            rec = Pick(s, t, w, kind, static_cast<uint32_t>(s.step) * 7u + 3u);
        if (!s.ok[w] || s.refused)
            rec = -1;
        if (rec >= 0 && !t.Line(rec))
            rec = -1;
        Cue c;
        c.who = w;
        c.kindNo = kind;
        c.ems = ems;
        if (rec < 0 && (kind == 12 || kind == 15 || kind == 16))
            return;                      // optional slots
        if (rec < 0) {
            c.kind = CueKind::kSayEffort;
            out.Add(c);
        } else {
            c.kind = CueKind::kSayRecord;
            c.rec = rec;
            c.dur = StepDur(t, rec);
            out.Add(c);
            s.spoke[w] = true;
            s.last[w] = rec;
            const double end = ems + c.dur;
            if (s.busy[w] < end)
                s.busy[w] = end;
            if (w == 1) {
                s.vline[0] = ems;
                s.vline[1] = end;
            }
            out.line.busy[w] = s.busy[w];
            out.line.vline[w] = s.vline[w];
            s.armed = true;
            s.checkAt = now + kCheckMs;
            s.checkW = w;
            s.checkKind = kind;
            s.tries = 1;
        }
        s.busy[w] += 100.0;
        out.line.busy[w] = s.busy[w];
    }

    // ext ivx_eff (the slam grunt / victim heavy take): up to three named
    // events, female speakers resolved by the owner.
    inline void SlamEffort(State& s, Result& out, int w, double ems) noexcept
    {
        Cue c;
        c.kind = w == 0 ? CueKind::kSlamGive : CueKind::kSlamTake;
        c.who = w;
        c.kindNo = w == 0 ? kSlamGive : kSlamTake;
        c.ems = ems;
        out.Add(c);
        (void)s;
    }

    struct TickIn {
        double   ems = 0.0;        // real ms of the move
        double   t0  = 0.0;        // move clock at the start
        uint64_t now = 0;          // wall clock for the speaking check
        bool     speaking = false; // DC is-speaking? answer, valid when checkDue
        bool     checkDue = false;
        bool     lookOff = false;
    };

    inline bool CheckDue(const State& s, uint64_t now) noexcept
    {
        return s.armed && s.on != 0 && now >= s.checkAt;
    }

    // ext intvox_tick after the timeline has been built: look-at refresh, one
    // step per planned moment, the victim's cut, the 140 ms refusal check and
    // the per-frame line feed for intfoley's voice clash / duck queries.
    inline void Tick(State& s, Result& out, const VoxTable& t, const TickIn& in) noexcept
    {
        const double ems = in.ems;
        out.line.on = s.on != 0;
        out.line.t0 = in.t0;
        out.line.shift = s.shift;
        out.line.step = s.step;
        out.line.count = s.nstep < 8 ? s.nstep : 8;
        for (int i = 0; i < out.line.count; ++i)
            out.line.steps[i].at = s.st[i].at;
        for (int w = 0; w < 2; ++w) {
            out.line.busy[w] = s.busy[w];
            out.line.vline[w] = s.vline[w];
        }
        if (!s.on)
            return;
        out.quietUntil = in.now + 4000;
        out.quietUntilSet = true;

        if (!in.lookOff && ems < s.killMs + 300.0)
            out.look = true;                 // the owner throttles to 600 ms

        while (s.step < s.nstep) {
            const Step& st = s.st[s.step];
            const int w = st.who, kind = st.kind;
            if (ems < st.at)
                break;
            if (kind < 20 && kind != 16 && ems < s.busy[w] - 1.0)
                break;                       // the same speaker is still talking
            ++s.step;
            out.line.step = s.step;
            if (kind == kSlamHit) {
                if (ems - st.at <= 400.0) {
                    Cue c;
                    c.kind = CueKind::kSlamHit;
                    c.who = 1;
                    c.ems = ems;
                    c.second = st.rec == -2;
                    out.Add(c);
                }
                continue;
            }
            if (kind >= 20) {
                if (ems - st.at <= 300.0)
                    SlamEffort(s, out, w, ems);
                continue;
            }
            if (EarlyKind(kind) && (st.at + 400.0 > s.killMs || ems > s.killMs - 200.0))
                continue;                    // too close to the kill
            if (ems - st.at > (kind == 16 ? 300.0 : 600.0))
                continue;                    // joined late
            if (kind == 11 || kind == 14)
                s.cutAt[1] = s.killMs - 80.0;
            else if (kind == 12)
                s.cutAt[1] = 0.0;
            Speak(s, out, t, w, kind, st.rec, ems, in.now);
        }

        if (s.cutAt[1] > 0.0 && ems >= s.cutAt[1]) {
            Cue c;
            c.kind = CueKind::kStopVictim;
            c.who = 1;
            c.ems = ems;
            out.Add(c);
            s.cutAt[1] = 0.0;
        }

        if (s.armed && in.checkDue && !s.refused && s.spoke[s.checkW] && !in.speaking) {
            s.armed = false;
            // One retry at priority 26 (the owner re-speaks the same record),
            // then the first demand of this interrogation can hand the whole
            // interrogation over to effort events. A late line does not.
            if (s.tries == 1) {
                ++s.tries;
                Cue c;
                c.kind = CueKind::kSayRecord;
                c.who = s.checkW;
                c.kindNo = s.checkKind;
                c.rec = s.last[s.checkW];
                c.ems = ems;
                c.dur = StepDur(t, c.rec);
                c.priority = 26;
                if (c.rec >= 0)
                    out.Add(c);
                s.checkAt = in.now + kCheckMs;
                s.armed = true;
            } else {
                // The first demand of this interrogation switches its voices
                // to effort events for the rest of the run; a late line only
                // re-speaks itself once as an effort event (ext's re-speaking
                // on a false negative, without latching g_ivx.refused).
                if (s.checkKind == 8 || s.checkKind == 13)
                    s.refused = true;
                Cue c;
                c.kind = CueKind::kSayEffort;
                c.who = s.checkW;
                c.kindNo = s.checkKind;
                c.ems = ems;
                out.Add(c);
            }
        }

        if (ems >= s.killMs - 80.0 && s.on == 1) {
            s.on = 2;
            Cue c;
            c.kind = CueKind::kStopVictim;
            c.who = 1;
            c.ems = ems;
            out.Add(c);
            out.lookClear = true;
            out.end = true;
            out.why = "kill frame";
        }
    }

    // ext's "no interrogation any more" cut: stop the two bodies, forget the
    // feed and clear the look-at. The match history survives for the next pair.
    inline void Cut(State& s, Result& out, const char* why) noexcept
    {
        if (!s.on)
            return;
        const bool spoke0 = s.spoke[0], spoke1 = s.spoke[1];
        const uint64_t e0 = s.ent[0], e1 = s.ent[1];
        s.on = 0;
        s.shift = 0.0;
        s.cutAt[0] = s.cutAt[1] = 0.0;
        for (int w = 0; w < 2; ++w) {
            const uint64_t ent = w == 0 ? e0 : e1;
            if ((w == 0 ? spoke0 : spoke1) && ent) {
                Cue c;
                c.kind = CueKind::kStopVictim;   // owner stops whichever body spoke
                c.who = w;
                out.Add(c);
            }
        }
        out.line = LineFeed{};
        out.lineClear = true;
        out.lookClear = true;
        out.end = true;
        out.why = why ? why : "pair ended";
    }

} // namespace net::audio::intvox::core
