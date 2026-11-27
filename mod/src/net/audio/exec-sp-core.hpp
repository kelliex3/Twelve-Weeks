#pragma once

// Pure execution-SP audio rules (tools/ext/execsp.c): the authored per-pair rows
// that fill the silent gaps of the 2013 execution timeline (attacker efforts and
// swing foley, victim takes, short melee pleas) and the unarmed contact events.
// There are no engine, audio, net, data or clock dependencies here: the native
// owner in exec-sp.cpp supplies the move clock, body handles, the DC replies and
// the sidecar answers. Tests: mod/tests/exec_sp_core_test.cpp.
//
// Honest gaps: the gun-exec pairs and the EXS_* label producer do not exist
// natively, so those entries are reachable only through BeginGun() /
// ContactFor() feeds. The SP event names and joint names are engine SIDs; no
// private PCM or generated table is needed for this layer.

#include <cstdint>
#include <cstring>

namespace net::audio::exec_sp::core {

    inline constexpr double kLateMs        = 250.0;  // ext's late-row cutoff
    inline constexpr double kOwnsVoiceMs   = 4500.0; // ext es_holds_victim window
    inline constexpr int    kMaxRowsPerPair = 31;    // ext's 32-bit done mask
    inline constexpr int    kMaxCues        = 32;

    enum class RowType : uint8_t { kSfx = 0, kVox = 1, kBeg = 2 };

    // ext ES_SFX / ES_VOX indices, kept as named values so a row never depends
    // on private generated header numbering.
    enum class Sfx : uint16_t {
        kSwishRW = 0, kSwishLE, kSwishRE, kFinWhoosh, kKneeSwish
    };
    enum class Vox : uint16_t {
        kGiveMed = 0, kGiveLrg, kGiveHvy, kTakeFace, kTakeBlnt, kTakeGut,
        kGrabbed, kDeathBlnt, kBreathe, kTakeShrp, kGurgle
    };

    struct Row {
        const char* pair;
        uint16_t    ms;     // ms from the 2013 clip start
        RowType     type;
        uint8_t     who;    // 0 attacker, 1 victim
        uint16_t    arg;    // SFX / VOX id, or earliest plea start for kBeg
    };

    // ext ESF / ESS2 / ES_ROWS, verbatim in timing and order. Contiguous rows of
    // one pair are selected by Begin(); the ext's first-key-wins event list is
    // kept separately below so a missing bank key can fall through.
#define TO1_ESF(P) \
        { P,  260, RowType::kVox, 1, static_cast<uint16_t>(Vox::kGrabbed) }, \
        { P, 1300, RowType::kBeg, 1, 720 }, \
        { P, 1100, RowType::kVox, 0, static_cast<uint16_t>(Vox::kGiveMed) }, \
        { P, 1130, RowType::kSfx, 0, static_cast<uint16_t>(Sfx::kSwishRW) }, \
        { P, 1560, RowType::kSfx, 0, static_cast<uint16_t>(Sfx::kSwishLE) }, \
        { P, 1650, RowType::kVox, 0, static_cast<uint16_t>(Vox::kGiveHvy) }, \
        { P, 1870, RowType::kVox, 1, static_cast<uint16_t>(Vox::kTakeBlnt) }, \
        { P, 2933, RowType::kBeg, 1, 2150 }, \
        { P, 2640, RowType::kSfx, 0, static_cast<uint16_t>(Sfx::kFinWhoosh) }, \
        { P, 2700, RowType::kVox, 0, static_cast<uint16_t>(Vox::kGiveHvy) }, \
        { P, 2965, RowType::kVox, 1, static_cast<uint16_t>(Vox::kDeathBlnt) }
#define TO1_ESS2(P) \
        { P,  420, RowType::kSfx, 0, static_cast<uint16_t>(Sfx::kKneeSwish) }, \
        { P,  470, RowType::kVox, 0, static_cast<uint16_t>(Vox::kGiveLrg) }, \
        { P,  660, RowType::kVox, 1, static_cast<uint16_t>(Vox::kTakeGut) }, \
        { P, 1080, RowType::kVox, 1, static_cast<uint16_t>(Vox::kTakeBlnt) }, \
        { P, 2233, RowType::kBeg, 1, 1450 }, \
        { P, 2020, RowType::kSfx, 0, static_cast<uint16_t>(Sfx::kKneeSwish) }, \
        { P, 2060, RowType::kVox, 0, static_cast<uint16_t>(Vox::kGiveHvy) }, \
        { P, 2265, RowType::kVox, 1, static_cast<uint16_t>(Vox::kDeathBlnt) }

    inline constexpr Row kRows[] = {
        // gun executions: the victim breathes / groans in the longest gaps
        { "pistol-execute-left",                 1000, RowType::kVox, 1, static_cast<uint16_t>(Vox::kBreathe) },
        { "9mm-execution-dlc5-right",            1000, RowType::kVox, 1, static_cast<uint16_t>(Vox::kBreathe) },
        { "9mm-execution-dlc5-left",              850, RowType::kVox, 1, static_cast<uint16_t>(Vox::kBreathe) },
        { "execution-revolver-left",             1000, RowType::kVox, 1, static_cast<uint16_t>(Vox::kBreathe) },
        { "execution-revolver-left",             2050, RowType::kVox, 1, static_cast<uint16_t>(Vox::kBreathe) },
        { "hunting-rifle-execution-dlc5-right",  1000, RowType::kVox, 1, static_cast<uint16_t>(Vox::kBreathe) },
        { "execution-bow-right",                 1850, RowType::kVox, 1, static_cast<uint16_t>(Vox::kTakeShrp) },
        { "long-gun-execute",                     850, RowType::kVox, 1, static_cast<uint16_t>(Vox::kTakeBlnt) },
        // unarmed two-punch, stomp and neckbreak
        TO1_ESF("fist-execute-enter-front"),
        TO1_ESF("fist-execute-enter-back"),
        TO1_ESS2("fist-stomp-execute-left"),
        TO1_ESS2("fist-stomp-execute-right"),
        { "fist-execute-neckbreak-fast-left",    1000, RowType::kVox, 1, static_cast<uint16_t>(Vox::kGrabbed) },
        { "fist-execute-neckbreak-fast-left",    1330, RowType::kVox, 0, static_cast<uint16_t>(Vox::kGiveHvy) },
        { "fist-execute-neckbreak-fast-right",   1000, RowType::kVox, 1, static_cast<uint16_t>(Vox::kGrabbed) },
        { "fist-execute-neckbreak-fast-right",   1330, RowType::kVox, 0, static_cast<uint16_t>(Vox::kGiveHvy) },
        { "fist-execute-neckbreak-fastest-left",  420, RowType::kVox, 1, static_cast<uint16_t>(Vox::kGrabbed) },
        { "fist-execute-neckbreak-fastest-left",  660, RowType::kVox, 0, static_cast<uint16_t>(Vox::kGiveHvy) },
        { "fist-execute-neckbreak-fastest-right", 420, RowType::kVox, 1, static_cast<uint16_t>(Vox::kGrabbed) },
        { "fist-execute-neckbreak-fastest-right", 660, RowType::kVox, 0, static_cast<uint16_t>(Vox::kGiveHvy) },
        // blunt: wind-ups, blunt takes, "bleeh" on the killing blow
        { "pipe-execute",       1300, RowType::kVox, 0, static_cast<uint16_t>(Vox::kGiveLrg) },
        { "pipe-execute",       1545, RowType::kVox, 1, static_cast<uint16_t>(Vox::kTakeBlnt) },
        { "pipe-execute",       1950, RowType::kVox, 1, static_cast<uint16_t>(Vox::kTakeGut) },
        { "pipe-execute",       2430, RowType::kVox, 0, static_cast<uint16_t>(Vox::kGiveHvy) },
        { "pipe-execute",       2665, RowType::kVox, 1, static_cast<uint16_t>(Vox::kDeathBlnt) },
        { "pipe-execute-left",   330, RowType::kVox, 0, static_cast<uint16_t>(Vox::kGiveLrg) },
        { "pipe-execute-left",   660, RowType::kVox, 1, static_cast<uint16_t>(Vox::kTakeGut) },
        { "pipe-execute-left",  1750, RowType::kVox, 1, static_cast<uint16_t>(Vox::kTakeBlnt) },
        { "pipe-execute-left",  2260, RowType::kVox, 0, static_cast<uint16_t>(Vox::kGiveHvy) },
        { "pipe-execute-left",  2535, RowType::kVox, 1, static_cast<uint16_t>(Vox::kDeathBlnt) },
        // blade: efforts, sharp take and a gurgle in the post-stab gap
        { "machete-decapitate", 1100, RowType::kVox, 1, static_cast<uint16_t>(Vox::kBreathe) },
        { "machete-decapitate", 1660, RowType::kVox, 0, static_cast<uint16_t>(Vox::kGiveLrg) },
        { "machete-execute-right", 1330, RowType::kVox, 0, static_cast<uint16_t>(Vox::kGiveLrg) },
        { "machete-execute-right", 1610, RowType::kVox, 1, static_cast<uint16_t>(Vox::kTakeShrp) },
        { "machete-execute-right", 1950, RowType::kVox, 1, static_cast<uint16_t>(Vox::kGurgle) },
        { "machete-execute-right", 2640, RowType::kVox, 0, static_cast<uint16_t>(Vox::kGiveMed) },
    };
#undef TO1_ESS2
#undef TO1_ESF

    inline constexpr int kRowCount = static_cast<int>(sizeof kRows / sizeof kRows[0]);

    struct SfxDesc { const char* ev; const char* joint; };
    inline constexpr SfxDesc kSfx[5] = {
        { "sfx-gear-swish-cotton", "r_wrist" },
        { "sfx-gear-swish-cotton", "l_elbow" },
        { "sfx-gear-swish-cotton", "r_elbow" },
        { "sfx-unarmed-finisher-whoosh", "r_wrist" },
        { "sfx-pants-swish-cotton", "r_knee" }
    };

    // ext ES_VOX's first-key-wins lists. The owner tries each name with `say`
    // until the engine answers (or all keys are exhausted).
    inline constexpr const char* const kVoxKeys[11][3] = {
        { "vox-eff-gen-fight-give-med", nullptr, nullptr },
        { "vox-eff-gen-fight-give-lrg-short", nullptr, nullptr },
        { "vox-eff-gen-fight-give-hvy-short", nullptr, nullptr },
        { "vox-eff-gen-fight-take-face", nullptr, nullptr },
        { "vox-eff-gen-fight-take-med-blnt", nullptr, nullptr },
        { "vox-eff-gen-fight-take-med-blnt-gut", nullptr, nullptr },
        { "vox-eff-gen-meatshield-grabbed-combat", nullptr, nullptr },
        { "vox-eff-gen-death-melee-blunt-large", nullptr, nullptr },
        { "vox-eff-gen-breath-fight-injured", "vox-eff-gen-brth-fight-injrd", "vox-eff-gen-brth-fight-med" },
        { "vox-eff-gen-fight-take-sml-shrp", "vox-eff-gen-fight-take-med-shrp", nullptr },
        { "vox-eff-gen-death-gurgle-blood-sml", "vox-eff-gen-death-gurgle-rattle", nullptr }
    };

    // ext ES_VOX[].vxe, in kVoxKeys order: VXE_GIVE / VXE_PAIN / VXE_DEATH.
    // The owner uses it to override a female speaker with her own effort take.
    inline constexpr int kVoxKind[11] = {
        0, 0, 0, 1, 1, 1, 1, 2, 1, 1, 1
    };

    inline bool StartsWith(const char* s, const char* prefix) noexcept
    {
        if (!s || !prefix)
            return false;
        while (*prefix) {
            if (*s != *prefix++)
                return false;
            ++s;
        }
        return true;
    }

    inline bool IsUnarmedName(const char* name) noexcept
    {
        return StartsWith(name, "fist-execute-enter")
            || StartsWith(name, "fist-stomp")
            || StartsWith(name, "fist-execute-neckbreak");
    }

    inline bool HasPleas(const char* name) noexcept
    {
        if (!name)
            return false;
        for (int i = 0; i < kRowCount; ++i)
            if (kRows[i].type == RowType::kBeg && kRows[i].pair && !std::strcmp(kRows[i].pair, name))
                return true;
        return false;
    }

    struct PleaRow {
        uint16_t earliestMs = 0;
        uint16_t cutMs      = 0;
        uint32_t salt       = 0;
    };

    // ext es_begin's ebm_setup loop: one scheduled take per EST_BEG row with
    // ebm_pick's 7 + n*13 salt. The owner arms them through exec_beg.
    inline int Pleas(const char* name, PleaRow out[kMaxRowsPerPair]) noexcept
    {
        if (!name)
            return 0;
        int n = 0;
        for (int i = 0; i < kRowCount && n < kMaxRowsPerPair; ++i) {
            if (kRows[i].type != RowType::kBeg || !kRows[i].pair
                || std::strcmp(kRows[i].pair, name))
                continue;
            out[n++] = { kRows[i].arg, kRows[i].ms, 7u + static_cast<uint32_t>(n - 1) * 13u };
        }
        return n;
    }

    // ext es_holds_victim's data half (the sidecar, live timeline and 4.5 s
    // window are read by the owner). The ext only claims the victim's voice for
    // a MELEE pair with a victim death row; gun rows just fill gaps around the
    // shot/plea voice.
    inline bool OwnsVictimVoice(const char* name, int kind) noexcept
    {
        if (!name || kind != 1)
            return false;
        for (int i = 0; i < kRowCount; ++i)
            if (!std::strcmp(kRows[i].pair, name)
                && kRows[i].type == RowType::kVox && kRows[i].who == 1
                && kRows[i].arg == static_cast<uint16_t>(Vox::kDeathBlnt))
                return true;
        return false;
    }

    inline bool NeedBluntWeapon(const char* name) noexcept
    {
        return StartsWith(name, "pipe-execute");
    }

    // ---- timeline state --------------------------------------------------------

    struct State {
        bool     on      = false;
        int      kind    = -1;     // ext g_ex.kind - 1: 0 gun, 1 melee
        int      pair    = -1;     // index into the producer's pair table
        int      lo      = 0;
        int      hi      = 0;
        double   t0      = 0.0;    // the pair's 0 ms on the shared move clock
        uint32_t done    = 0;
        uint64_t uid     = 0;      // paired-move dedupe key
        bool     ownVic  = false;  // ext g_es.own_vic
        int      played  = 0;
        int      skipped = 0;
    };

    inline void Reset(State& s) noexcept { s = State{}; }

    // `t0` is ext's g_ex.t0 captured at move start (the ext's skip_ms is only
    // logged, not folded into the clock). `pair` is the producer's table index.
    inline bool Begin(State& s, int kind, int pair, const char* name, double t0,
                      bool off, bool bluntOk, uint64_t uid) noexcept
    {
        Reset(s);
        if (off || !name || (kind != 0 && kind != 1))
            return false;
        if (kind == 1 && NeedBluntWeapon(name) && !bluntOk)
            return false;
        int lo = -1, hi = -1;
        for (int i = 0; i < kRowCount; ++i) {
            if (!std::strcmp(kRows[i].pair, name)) {
                if (lo < 0)
                    lo = i;
                hi = i + 1;
            }
        }
        if (lo < 0 || hi - lo > kMaxRowsPerPair)
            return false;
        s.on = true;
        s.kind = kind;
        s.pair = pair;
        s.lo = lo;
        s.hi = hi;
        s.t0 = t0;
        s.uid = uid;
        s.ownVic = OwnsVictimVoice(name, kind);
        return true;
    }

    // ext es_tick's pair guard. The owner uses this before feeding Tick(), and
    // Cut() when the move changes or the session ends.
    inline bool Matches(const State& s, int kind, int pair, uint64_t uid) noexcept
    {
        return s.on && s.kind == kind && s.pair == pair && s.uid == uid;
    }

    inline void Cut(State& s) noexcept { s.on = false; }

    // ext iw_copies_shift: an intwarp hold shifts the execution clocks together.
    // No native intwarp producer feeds this yet; the feed exists so one can be
    // added without duplicating the clock rule.
    inline void Shift(State& s, double fromT0, double deltaMs) noexcept
    {
        if (s.on && s.t0 == fromT0)
            s.t0 += deltaMs;
    }

    enum class CueKind : uint8_t { kSfx, kVox };

    struct Cue {
        CueKind     kind = CueKind::kSfx;
        uint64_t    ent  = 0;
        const char* name = nullptr; // SFX event / first VOX key
        const char* joint = nullptr;
        int         voice = -1;
        double      ems   = 0.0;
    };

    struct Result {
        Cue  cues[kMaxCues];
        int  count = 0;
        bool cut   = false;
    };

    inline void AddCue(Result& r, Cue cue) noexcept
    {
        if (r.count < kMaxCues)
            r.cues[r.count++] = cue;
    }

    struct TickInput {
        double   moveMs       = 0.0;
        uint64_t attEnt       = 0;
        uint64_t vicEnt       = 0;
        bool     gunVoiceCovered = false; // ext eb_covers_gun()
    };

    // ext es_tick: rows fire on the shared 2013 move clock, one shot per row.
    // A row more than 250 ms late is skipped; EST_BEG rows are handled at
    // begin time by exec_beg's scheduled-plea API, not here.
    inline Result Tick(State& s, const TickInput& in) noexcept
    {
        Result r;
        if (!s.on)
            return r;
        const double ems = in.moveMs - s.t0;
        for (int i = s.lo; i < s.hi; ++i) {
            const uint32_t bit = 1u << static_cast<uint32_t>(i - s.lo);
            const Row& row = kRows[i];
            if ((s.done & bit) || row.type == RowType::kBeg
                || ems < static_cast<double>(row.ms))
                continue;
            s.done |= bit;
            const double late = ems - static_cast<double>(row.ms);
            if (late > kLateMs) {
                ++s.skipped;
                continue;
            }
            const uint64_t ent = row.who ? in.vicEnt : in.attEnt;
            if (!ent || ent == ~0ULL) {
                ++s.skipped;
                continue;
            }
            if (row.type == RowType::kSfx) {
                const int id = static_cast<int>(row.arg);
                if (id < 0 || id >= static_cast<int>(sizeof kSfx / sizeof kSfx[0])) {
                    ++s.skipped;
                    continue;
                }
                AddCue(r, { CueKind::kSfx, ent, kSfx[id].ev, kSfx[id].joint, -1, ems });
                ++s.played;
            }
            else {
                // A gun execution's plea / shot grunts own the victim voice.
                if (row.who == 1 && in.gunVoiceCovered) {
                    ++s.skipped;
                    continue;
                }
                const int id = static_cast<int>(row.arg);
                if (id < 0 || id >= static_cast<int>(sizeof kVoxKeys / sizeof kVoxKeys[0])
                    || !kVoxKeys[id][0]) {
                    ++s.skipped;
                    continue;
                }
                AddCue(r, { CueKind::kVox, ent, kVoxKeys[id][0], nullptr, id, ems });
                ++s.played;
            }
        }
        return r;
    }

    // ---- contacts (ext ex_play_label -> es_contact) ----------------------------

    enum class ContactLabel : int {
        kHitFistFace = 0, kHitFistFaceFinisher, kGutKick,
        kHitBootHead, kHitBootFace, kBreakNeck
    };

    struct Contact {
        const char* ev    = nullptr;
        const char* joint = nullptr;
    };

    // The native port has no ex_play_label producer; this is the feed's pure
    // mapper. The 2nd fist blow is not a kill, so the finisher face skin is used
    // only after 2.5 s (ext's ems check).
    inline bool ContactFor(ContactLabel label, const char* pair, double ems,
                           Contact& out) noexcept
    {
        if (!IsUnarmedName(pair))
            return false;
        switch (label) {
        case ContactLabel::kHitFistFace:
            out = { "sfx-unarmed-hit-face-skin", "heada" };
            return true;
        case ContactLabel::kHitFistFaceFinisher:
            out = { ems > 2500.0 ? "sfx-unarmed-finisher-face-skin"
                                : "sfx-unarmed-hit-face-skin", "heada" };
            return true;
        case ContactLabel::kGutKick:
            if (!StartsWith(pair, "fist-stomp"))
                return false;
            out = { "sfx-unarmed-hit-gut", "pelvis" };
            return true;
        case ContactLabel::kHitBootHead:
        case ContactLabel::kHitBootFace:
            out = { "sfx-unarmed-hit-kick-face-skin", "heada" };
            return true;
        case ContactLabel::kBreakNeck:
            out = { "sfx-unarmed-finisher-neckbreak-combat", "headb" };
            return true;
        default:
            return false;
        }
    }

} // namespace net::audio::exec_sp::core
