#pragma once

// Pure execution-begging rules (tools/ext/exec_beg.c), with no engine, audio, net,
// data or window headers and no clock of its own: the native owner in
// exec-beg.cpp supplies nowMs, the paired-move clock, match liveness, the sidecar
// answers, the DC replies and the private vox table. Kept exactly where the data
// exists:
//   * the plea starts EB_LEAD_MS before the first shot (180..800 ms, never sooner
//     than 80 ms into the move), each non-final shot cuts it and starts one hit
//     grunt, and the death shot only cuts (arming a headshot death take when the
//     last shot was aimed at the head);
//   * the dismember scream chain uses EB_SCR_SEQ and each take's voiced length *
//     0.93, falling through the same kind-fallback table;
//   * the deterministic pick hash, the language / long-shout filters and the
//     140 ms is-speaking? fallback to effort events;
//   * the music duck (0.35 from beg-100 ms to last shot + 350 ms), the plea mixer
//     snapshot window, the melee-plea boost window and the 8 s voice cover.
// Tests: mod/tests/exec_beg_core_test.cpp.

#include <cstdint>

namespace net::audio::exec_beg::core {

    inline constexpr uint64_t kPleaSnap     = 0x334b0a906e6eb50eULL; // ext EB_PLEA_SNAP
    inline constexpr uint64_t kLeadMs      = 800;   // ext EB_LEAD_MS
    inline constexpr uint64_t kCheckMs     = 140;   // ext EB_CHECK_MS
    inline constexpr uint64_t kMaxCoverMs  = 8000;  // ext eb_tick's cover
    inline constexpr float    kMusicDuck   = 0.35f; // ext EB_MUSIC_DUCK
    inline constexpr int      kMaxShots    = 6;     // ext g_eb.shot[6]
    inline constexpr int      kScreamSteps = 12;    // ext EB_SCR_SEQ
    inline constexpr int      kMeleeMaxDur = 1100;  // ext EBM_MAXDUR
    inline constexpr int      kFinisherMaxDur = 800;// ext EBF_MAXDUR
    inline constexpr int      kMeleePleaSalt = 7;   // ext execsp.c's first plea salt
    inline constexpr int      kMaxMeleePleas = 3;   // ext execsp.c g_es.beg[3]

    // ext EB_EV: effort-event fallbacks per kind. Empty entries end the list.
    inline constexpr const char* const kEffortEvents[8][3] = {
        { "vox-eff-gen-fight-take-hvy", "vox-eff-gen-begs-for-life-stun", nullptr },
        { "vox-eff-gen-hit-by-bullet", "vox-eff-gen-fight-take-med", nullptr },
        { "vox-eff-gen-hit-by-rifle", "vox-eff-gen-hit-by-bullet", nullptr },
        { "vox-eff-gen-hit-by-shotgun", "vox-eff-gen-hit-by-bullet", nullptr },
        { "vox-eff-gen-hit-by-explosion", "vox-eff-gen-death-dismember", "vox-eff-gen-fight-take-hvy" },
        { "vox-eff-gen-fight-take-hvy", "vox-eff-gen-fight-take-lrg", nullptr },
        { "vox-eff-gen-death-gurgle-whine-lrg", "vox-eff-gen-death-gurgle-whine-sml", nullptr },
        { "vox-eff-gen-brth-fight-injrd", "vox-eff-gen-brth-fight-med", nullptr }
    };

    enum class VoiceKind : int {
        kBeg = 0, kPistol = 1, kRifle = 2, kShotgun = 3,
        kAgony = 4, kShortScream = 5, kCrying = 6, kPanting = 7
    };

    enum class CueKind : int { kNone = 0, kSpeakRecord, kSpeakEvent, kStop, kHeadshot };

    // The generated vox_beg.h tables in a flat, bounds-checked form. kRange is
    // [fac][female][voice][kind][2] row-major; the private data header supplies
    // exactly that layout. A null avail means "no active-language filter" (ext's
    // NULL VXG_AVAIL table), while a null table means "no private data".
    struct VoxTable {
        const uint64_t* lines  = nullptr;  // VXG_LINES
        const int*      range  = nullptr;  // VXG_RANGE, flattened as above
        const int*      dur    = nullptr;  // VXG_DUR (ms)
        const uint8_t*  scream = nullptr;  // VXG_SCREAM
        const uint8_t*  avail  = nullptr;  // VXG_AVAIL_<language>, nullptr = all
        int             count  = 0;        // VXG_N

        int Range(int fac, int female, int voice, int kind, int field) const noexcept
        {
            if (!range || fac < 0 || fac > 1 || female < 0 || female > 1 ||
                voice < 0 || voice > 3 || kind < 0 || kind > 7 || field < 0 || field > 1)
                return 0;
            return range[(((fac * 2 + female) * 4 + voice) * 8 + kind) * 2 + field];
        }
        uint64_t Line(int rec) const noexcept
        {
            return lines && rec >= 0 && rec < count ? lines[rec] : 0;
        }
        int Dur(int rec) const noexcept
        {
            return dur && rec >= 0 && rec < count ? dur[rec] : 0;
        }
        bool Scream(int rec) const noexcept
        {
            return scream && rec >= 0 && rec < count && scream[rec] != 0;
        }
        bool Avail(int rec) const noexcept
        {
            return !avail || (rec >= 0 && rec < count && avail[rec] != 0);
        }
    };

    // ext eb_pick's wheel: (seed ^ kind*0x9e3779b1 ^ salt*0x85ebca6b) * 2654435761,
    // then two avalanche rounds.
    inline uint32_t PickHash(uint32_t seed, int kind, uint32_t salt) noexcept
    {
        uint32_t hs = (seed ^ (static_cast<uint32_t>(kind) * 0x9e3779b1u)
                            ^ (salt * 0x85ebca6bu)) * 2654435761u;
        hs ^= hs >> 15;
        hs *= 2246822519u;
        hs ^= hs >> 13;
        return hs;
    }

    // ext's FB[8][5]: the plea, the three hit grunts and the four scream / cry
    // kinds with their fallbacks (-1 ends the row).
    inline int Fallback(int kind, int slot) noexcept
    {
        static constexpr int8_t FB[8][5] = {
            { 0, -1 }, { 1, 2, 3, -1 }, { 2, 3, 1, -1 }, { 3, 1, 2, -1 },
            { 4, 5, 6, 3, -1 }, { 5, 4, 6, 3, -1 },
            { 6, 5, 4, 3, -1 }, { 7, 6, 5, 3, -1 }
        };
        if (kind < 0 || kind > 7 || slot < 0 || slot > 4)
            return -1;
        return FB[kind][slot];
    }

    inline int ScreamSequence(int step) noexcept
    {
        static constexpr uint8_t SEQ[kScreamSteps] = { 5, 5, 5, 5, 4, 6, 7, 5, 6, 7, 6, 7 };
        if (step < kScreamSteps)
            return SEQ[step];
        return SEQ[kScreamSteps - 1 - (step & 1)];
    }

    // ext eb_pick. Only the verbal plea (kind 0) checks the active language and
    // drops VXG_SCREAM takes unless t2r-exec-keep-long-scream.txt is present;
    // pickMax (EBM_MAXDUR for melee pleas) applies only to that kind too.
    inline int Pick(const VoxTable& t, uint32_t seed, int fac, int female, int voice,
                    int kind, int avoid, uint32_t salt, bool keepScream,
                    int pickMax) noexcept
    {
        if (kind < 0 || kind > 7)
            return -1;
        const uint32_t hs = PickHash(seed, kind, salt);
        for (int slot = 0; slot < 5; ++slot) {
            const int k = Fallback(kind, slot);
            if (k < 0)
                break;
            const int first = t.Range(fac, female, voice, k, 0);
            const int n = t.Range(fac, female, voice, k, 1);
            const bool verbal = (k == 0);
            const bool dropScream = verbal && !keepScream;
            int cand[16];
            int nc = 0;
            for (int i = 0; i < n && nc < 16; ++i) {
                const int rec = first + i;
                if (rec < 0 || rec >= t.count)
                    continue;
                if (dropScream && t.Scream(rec))
                    continue;
                if (verbal && pickMax > 0 && t.Dur(rec) > pickMax)
                    continue;
                if (!verbal || t.Avail(rec))
                    cand[nc++] = rec;
            }
            if (!nc)
                continue;
            int ci = static_cast<int>(hs % static_cast<uint32_t>(nc));
            if (nc > 1 && cand[ci] == avoid)
                ci = (ci + 1) % nc;
            return cand[ci];
        }
        return -1;
    }

    // ext eb_fin_gasp's own two-pass pick: never a scream, short takes first,
    // active-language stream required when the table is known.
    inline int FinGaspPick(const VoxTable& t, int fac, int female, int voice,
                           uint32_t seed) noexcept
    {
        if (fac < 0 || fac > 1 || female < 0 || female > 1 || voice < 0 || voice > 3)
            return -1;
        const int first = t.Range(fac, female, voice, 0, 0);
        const int n = t.Range(fac, female, voice, 0, 1);
        int cand[16];
        int nc = 0;
        for (int pass = 0; pass < 2 && !nc; ++pass) {
            for (int i = 0; i < n && nc < 16; ++i) {
                const int rec = first + i;
                if (rec < 0 || rec >= t.count || t.Scream(rec) || !t.Avail(rec))
                    continue;
                if (!pass && t.Dur(rec) > kFinisherMaxDur)
                    continue;
                cand[nc++] = rec;
            }
        }
        if (!nc)
            return -1;
        // ext's hash is PickHash(seed, 1, 0): seed ^ 0x9e3779b1.
        const uint32_t hs = PickHash(seed, 1, 0);
        return cand[hs % static_cast<uint32_t>(nc)];
    }

    // ---- state ----------------------------------------------------------------

    struct State {
        bool     on          = false;
        bool     events      = false;   // dialogue refused (or no voice data): effort events
        bool     keepScream  = false;   // t2r-exec-keep-long-scream.txt
        int      pickMax     = 0;       // EBM_MAXDUR while picking a melee plea
        int      pair        = -1;
        int      nshot       = 0;
        int      shotI       = 0;
        uint16_t shot[kMaxShots] = {};
        int      limb        = -1;
        bool     lastHead    = false;
        int      pkind       = static_cast<int>(VoiceKind::kPistol);
        int      fac         = -1;      // -1 = unknown (no native VXG faction / look mapping)
        int      female      = -1;
        int      voice       = -1;
        uint32_t seed        = 0;
        double   t0          = 0.0;     // paired-move ms at the start (minus skip)
        uint64_t ent         = 0;
        int      lastBeg     = -1;
        int      lastPain    = -1;
        uint32_t begAt       = 0;
        bool     begDone     = false;
        bool     speaking    = false;
        uint64_t checkAt     = 0;
        int      checkKind   = 0;
        int      checkRec    = -1;
        bool     checkEv     = false;
        bool     scrOn       = false;
        int      scrStep     = 0;
        int      scrN        = 0;
        int      scrLast     = -1;
        double   scrNext     = 0.0;
        double   mwFrom      = 0.0;     // melee plea boost window
        double   mwTo        = 0.0;
        bool     ducked      = false;   // eb_music_duck's static `ducked`
        uint32_t activeUid   = 0;       // paired-move dedupe key (owner-owned)

        // execsp.c's EST_BEG rows schedule up to three short pleas on the move
        // clock (ebm_pick -> start = cut - duration * 0.72, floored by the row's
        // earliest start); Tick() speaks one at its start and cuts it at the blow.
        struct MeleePlea {
            int    rec     = -1;
            double start   = 0.0;
            double cut     = 0.0;
            bool   said    = false;
            bool   stopped = false;
        };
        MeleePlea plea[kMaxMeleePleas];
        int       nplea = 0;
    };

    struct Cue {
        CueKind  kind       = CueKind::kNone;
        uint64_t ent        = 0;
        uint64_t line       = 0;        // kSpeakRecord: VXG_LINES[rec]
        int      voiceKind  = 0;        // kSpeakEvent: which EB_EV row
        int      eventIndex = 0;        // log only
    };

    struct Result {
        Cue         cues[24];
        int         count        = 0;
        bool        duckChanged  = false; // call exec_music::SetDuck
        bool        duckWant     = false; // true = 0.35, false = 1.0
        bool        boost        = false; // per-frame mixer snapshot
        bool        checkDue     = false; // owner asked is-speaking?
        const char* cutWhy       = nullptr;
    };

    inline void Reset(State& s) noexcept { s = State{}; }

    inline void AddCue(Result& r, Cue cue) noexcept
    {
        if (r.count < static_cast<int>(sizeof r.cues / sizeof r.cues[0]))
            r.cues[r.count++] = cue;
    }

    inline void SetCheck(State& s, uint64_t nowMs, int kind, int rec, bool ev) noexcept
    {
        s.checkAt   = nowMs + kCheckMs;
        s.checkKind = kind;
        s.checkRec  = rec;
        s.checkEv   = ev;
    }

    // Ext eb_say: the record path until the engine refuses it, then the named
    // effort events. The owner performs the DC call and reports success.
    inline void EmitSpeak(State& s, Result& r, const VoxTable& t, uint64_t nowMs,
                          int kind, int rec) noexcept
    {
        if (!s.ent || s.ent == ~0ULL)
            return;
        if (s.events || rec < 0) {
            AddCue(r, { CueKind::kSpeakEvent, s.ent, 0, kind, 0 });
            SetCheck(s, nowMs, kind, -1, true);
            return;
        }
        const uint64_t line = t.Line(rec);
        if (!line)
            return;
        AddCue(r, { CueKind::kSpeakRecord, s.ent, line, kind, 0 });
        SetCheck(s, nowMs, kind, rec, false);
    }

    inline void EmitStop(State& s, Result& r) noexcept
    {
        if (!s.speaking || !s.ent)
            return;
        AddCue(r, { CueKind::kStop, s.ent, 0, 0, 0 });
        s.speaking = false;
    }

    inline void CommitSpeak(State& s, bool started) noexcept
    {
        s.speaking = started && s.ent != 0 && s.ent != ~0ULL;
    }

    inline bool CheckDue(const State& s, uint64_t nowMs) noexcept
    {
        return s.checkAt != 0 && nowMs >= s.checkAt;
    }

    // ext eb_cut / the match and cover exits. Echo the duck restore before the
    // state is forgotten so the owner puts exec music back up.
    inline void Cut(State& s, Result& r, const char* why) noexcept
    {
        if (s.ducked) {
            r.duckChanged = true;
            r.duckWant = false;
        }
        EmitStop(s, r);
        if (s.on)
            r.cutWhy = why;
        Reset(s);
    }

    // ---- begins ---------------------------------------------------------------

    struct GunBegin {
        uint64_t       ent       = 0;
        int            pair      = -1;
        int            fac       = -1;
        int            female    = -1;
        int            voice     = -1;
        uint32_t       seed      = 0;
        double         moveMs    = 0.0;
        float          skipMs    = 0.0;
        int            weaponClass = 1;  // 1 pistol, 2 rifle / long, 3 shotgun
        const uint16_t* shots    = nullptr; // non-arrow shots, ext g_eb.shot[]
        int            shotCount = 0;
        int            limb      = -1;      // index into shots, ext g_eb.limb
        bool           lastHead  = false;
    };

    // ext eb_begin, after the missing gun-execution producer has supplied the
    // shot schedule, aim flags and voice set. forceEvents lets a future gun feed
    // run the effort path before the private take table is available.
    inline bool BeginGun(State& s, const VoxTable& t, const GunBegin& in,
                         bool off, bool keepScream, bool forceEvents) noexcept
    {
        Reset(s);
        if (off || !in.shots || in.shotCount <= 0 ||
            !in.ent || in.ent == ~0ULL ||
            in.fac < 0 || in.fac > 1 ||
            in.female < 0 || in.female > 1 || in.voice < 0 || in.voice > 3)
            return false;
        s.keepScream = keepScream;
        s.events = forceEvents;
        s.ent = in.ent;
        s.pair = in.pair;
        s.fac = in.fac;
        s.female = in.female;
        s.voice = in.voice;
        s.seed = in.seed;
        s.t0 = in.moveMs - static_cast<double>(in.skipMs);
        s.pkind = in.weaponClass == 3 ? static_cast<int>(VoiceKind::kShotgun)
                : in.weaponClass == 2 ? static_cast<int>(VoiceKind::kRifle)
                : static_cast<int>(VoiceKind::kPistol);
        const int n = in.shotCount < kMaxShots ? in.shotCount : kMaxShots;
        for (int i = 0; i < n; ++i)
            s.shot[i] = in.shots[i];
        s.nshot = n;
        s.limb = in.limb >= 0 && in.limb < n ? in.limb : -1;
        s.lastHead = in.lastHead;
        const int bc = Pick(t, s.seed, s.fac, s.female, s.voice, 0, -1, 0,
                            keepScream, 0);
        if (bc < 0 && !s.events)
            return false;
        s.lastBeg = bc;
        int lead = bc >= 0 ? static_cast<int>(t.Dur(bc) * 0.8f)
                           : static_cast<int>(kLeadMs);
        if (lead > static_cast<int>(kLeadMs))
            lead = static_cast<int>(kLeadMs);
        if (lead < 180)
            lead = 180;
        int at = static_cast<int>(s.shot[0]) - lead;
        if (at < 80)
            at = 80;
        s.begAt = static_cast<uint32_t>(at);
        s.on = true;
        return true;
    }

    struct MeleeBegin {
        uint64_t ent    = 0;
        int      pair   = -1;
        int      fac    = -1;
        int      female = -1;
        int      voice  = -1;
        uint32_t seed   = 0;
        double   moveMs = 0.0;
    };

    // ext ebm_setup. The native paired feed has no faction / look / voice
    // mapping, so forceEvents is the live path there; the record path stays
    // dormant until that mapping and the private VXG table are supplied.
    inline bool BeginMelee(State& s, const MeleeBegin& in, bool off,
                           bool meleeOff, bool keepScream, bool forceEvents) noexcept
    {
        Reset(s);
        if (off || meleeOff || !in.ent || in.ent == ~0ULL)
            return false;
        s.keepScream = keepScream;
        s.events = forceEvents;
        s.ent = in.ent;
        s.pair = in.pair;
        s.fac = in.fac;
        s.female = in.female;
        s.voice = in.voice;
        s.seed = in.seed;
        s.t0 = in.moveMs;
        s.pickMax = kMeleeMaxDur;
        s.on = true;
        return true;
    }

    // ext ebm_say / es_tick's EST_BEG row. `cutMs` is the authored blow moment;
    // the native bridge only has the move's kill frame, so it supplies that.
    inline void BeginMeleePlea(State& s, Result& r, const VoxTable& t,
                               uint64_t nowMs, double ems, double cutMs,
                               uint32_t salt) noexcept
    {
        if (!s.on)
            return;
        const int rec = s.events ? -1
            : Pick(t, s.seed, s.fac, s.female, s.voice, 0, s.lastBeg, salt,
                   s.keepScream, kMeleeMaxDur);
        s.lastBeg = rec;
        s.mwFrom = ems;
        s.mwTo = cutMs + 60.0;
        EmitSpeak(s, r, t, nowMs, static_cast<int>(VoiceKind::kBeg), rec);
    }

    inline void SetMeleeWindow(State& s, double from, double to) noexcept
    {
        s.mwFrom = from;
        s.mwTo = to + 60.0;
    }

    inline void ClearMeleePleas(State& s) noexcept
    {
        s.nplea = 0;
        for (int i = 0; i < kMaxMeleePleas; ++i)
            s.plea[i] = {};
    }

    // ext execsp.c's EST_BEG rows: pick a short take with ebm_pick's salt, then
    // schedule it to start `dur * 0.72` before the blow but never before the
    // row's authored earliest start. The DC call happens in Tick(), not here,
    // so a joined-late run can skip a stale row exactly like the ext.
    inline bool ScheduleMeleePlea(State& s, Result& r, const VoxTable& t,
                                  double ems, double earliestMs, double cutMs,
                                  uint32_t salt) noexcept
    {
        (void)r;
        if (!s.on || s.nplea >= kMaxMeleePleas || cutMs <= ems)
            return false;
        const int prev = s.nplea > 0 ? s.plea[s.nplea - 1].rec : -1;
        const int rec = s.events ? -1
            : Pick(t, s.seed, s.fac, s.female, s.voice, 0, prev, salt,
                   s.keepScream, kMeleeMaxDur);
        const double dur = (rec >= 0 && t.Dur(rec) > 0)
            ? static_cast<double>(t.Dur(rec)) : 600.0;
        double start = cutMs - dur * 0.72;
        if (start < earliestMs)
            start = earliestMs;
        s.plea[s.nplea] = { rec, start, cutMs, false, false };
        ++s.nplea;
        return true;
    }

    // ---- per-frame rules ------------------------------------------------------

    inline bool DuckWanted(const State& s, double ems) noexcept
    {
        if (!s.on || s.nshot <= 0)
            return false;
        return ems >= static_cast<double>(s.begAt) - 100.0
            && ems <= static_cast<double>(s.shot[s.nshot - 1]) + 350.0;
    }

    // ext eb_covers_gun: the plea / grunts hold the victim's voice. A melee
    // setup (no shots) covers only while its scheduled take's boost window is
    // live; a gun setup covers from shortly before the plea onwards.
    inline bool CoversGun(const State& s, double ems) noexcept
    {
        if (!s.on)
            return false;
        if (s.nshot <= 0)
            return s.mwTo > 0.0
                && ems >= s.mwFrom - 150.0 && ems <= s.mwTo;
        return ems >= static_cast<double>(s.begAt) - 150.0;
    }

    inline bool BoostWanted(const State& s, double ems, bool pleaBoostOff,
                            bool meleeBoostAllowed) noexcept
    {
        if (!s.on || pleaBoostOff)
            return false;
        if (s.nshot <= 0) {
            if (!meleeBoostAllowed || s.mwTo <= s.mwFrom)
                return false;
            return ems >= s.mwFrom - 100.0 && ems <= s.mwTo;
        }
        const double until = s.limb >= 0
            ? static_cast<double>(s.shot[s.nshot - 1]) + 60.0
            : static_cast<double>(s.shot[0]) + 60.0;
        return ems >= static_cast<double>(s.begAt) - 100.0 && ems <= until;
    }

    struct TickInput {
        uint64_t    nowMs            = 0;
        double      moveMs           = 0.0;
        bool        matchLive        = false;
        bool        pleaBoostOff     = false;  // t2r-no-plea-boost.txt
        bool        meleeBoostAllowed = false; // t2r-melee-plea-boost.txt
        bool        musicLive        = false;  // exec music cue on, not fading
        uint64_t    checkReply       = ~0ULL;  // ~0 = native not callable / not due
        VoxTable    table;
    };

    inline void EmitScreamStep(State& s, Result& r, const VoxTable& t,
                               double ems, uint64_t nowMs) noexcept
    {
        const int kind = ScreamSequence(s.scrStep);
        const int rec = Pick(t, s.seed, s.fac, s.female, s.voice, kind,
                             s.scrLast, static_cast<uint32_t>(s.scrStep),
                             s.keepScream, 0);
        if (rec < 0) {
            s.scrOn = false;
            return;
        }
        s.scrLast = rec;
        EmitSpeak(s, r, t, nowMs, kind, rec);
        const int dur = t.Dur(rec) ? t.Dur(rec) : 700;
        s.scrNext = ems + static_cast<double>(dur) * 0.93;
        ++s.scrStep;
        ++s.scrN;
    }

    // ext eb_tick, in the same order: duck / boost first, the is-speaking? check,
    // the match and 8 s cuts, the plea, the shot loop, then the scream chain.
    inline Result Tick(State& s, const TickInput& in) noexcept
    {
        Result r;
        const double ems = in.moveMs - s.t0;

        const bool want = DuckWanted(s, ems);
        if (want != s.ducked) {
            if (!in.musicLive) {
                s.ducked = false;        // ext: a < 0 forgets the duck
            } else {
                r.duckChanged = true;
                r.duckWant = want;
                s.ducked = want;
            }
        }
        r.boost = BoostWanted(s, ems, in.pleaBoostOff, in.meleeBoostAllowed);

        if (CheckDue(s, in.nowMs)) {
            s.checkAt = 0;
            r.checkDue = true;
            // ~0 = the native is not callable; a spoken line has a non-zero low
            // byte. Only an explicit "no" switches the session to effort events.
            if (in.checkReply != ~0ULL && (in.checkReply & 0xffULL) == 0
                && !s.checkEv && !s.events) {
                s.events = true;
                EmitSpeak(s, r, in.table, in.nowMs, s.checkKind, -1);
            }
        }

        if (!s.on)
            return r;
        if (!in.matchLive) {
            Cut(s, r, "match left the playing state");
            return r;
        }
        if (ems > static_cast<double>(kMaxCoverMs)) {
            EmitStop(s, r);
            s.on = false;
            return r;
        }

        // execsp EST_BEG: speak at the scheduled start (late rows are dropped),
        // stop at the blow. This shares the same single voice slot as the gun
        // plea, so the stop cue only fires when a line actually started.
        for (int i = 0; i < s.nplea; ++i) {
            State::MeleePlea& p = s.plea[i];
            if (!p.said && ems >= p.start) {
                p.said = true;
                if (ems - p.start < 250.0 && ems < p.cut - 60.0) {
                    EmitSpeak(s, r, in.table, in.nowMs,
                              static_cast<int>(VoiceKind::kBeg), p.rec);
                    SetMeleeWindow(s, ems, p.cut);
                }
                else
                    p.stopped = true;
            }
            if (p.said && !p.stopped && ems >= p.cut) {
                p.stopped = true;
                EmitStop(s, r);
            }
        }

        if (!s.begDone && ems >= static_cast<double>(s.begAt)) {
            s.begDone = true;
            if (ems - static_cast<double>(s.begAt) < 350.0
                && ems < static_cast<double>(s.shot[0]) - 120.0)
                EmitSpeak(s, r, in.table, in.nowMs,
                          static_cast<int>(VoiceKind::kBeg), s.lastBeg);
        }

        for (; s.shotI < s.nshot
               && static_cast<double>(s.shot[s.shotI]) <= ems; ++s.shotI) {
            const int i = s.shotI;
            const bool last = (i == s.nshot - 1);
            const double late = ems - static_cast<double>(s.shot[i]);

            EmitStop(s, r);              // the shot interrupts whatever was speaking
            if (last) {
                if (s.lastHead)
                    AddCue(r, { CueKind::kHeadshot, s.ent, 0, 0, 0 });
                if (s.scrOn)
                    s.scrOn = false;
                continue;
            }
            if (i == s.limb && late < 400.0) {
                s.scrOn = true;
                s.scrStep = 0;
                s.scrN = 0;
                s.scrLast = -1;
                EmitScreamStep(s, r, in.table, ems, in.nowMs);
                continue;
            }
            if (late > 200.0)
                continue;                // a stalled frame: the moment has passed
            const int pc = Pick(in.table, s.seed, s.fac, s.female, s.voice,
                                s.pkind, s.lastPain, 100u + static_cast<uint32_t>(s.shotI),
                                s.keepScream, 0);
            if (pc < 0)
                continue;
            s.lastPain = pc;
            EmitSpeak(s, r, in.table, in.nowMs, s.pkind, pc);
        }

        if (s.scrOn && ems >= s.scrNext
            && ems < static_cast<double>(s.shot[s.nshot - 1]))
            EmitScreamStep(s, r, in.table, ems, in.nowMs);
        return r;
    }

} // namespace net::audio::exec_beg::core
