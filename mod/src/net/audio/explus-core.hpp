#pragma once

// Pure execution-foley-plus rules (tools/ext/explus.c): the generated per-pair
// layer timeline, the shared mixer budgets / gain model / voice gates, and the
// recipes used by the other melee systems. No engine, audio, net, clock, sidecar
// or private-data read happens here: the native owner in explus.cpp supplies the
// data view, the clocks, the sidecar answers and the PCM/say sinks.
//
// The generated tables (execplus_tl.h / execplus.h / footsteps.h, the waves and
// the PCM blobs) are private and do not ship here. They enter through DataView
// from explus-data.hpp / footsteps-data.hpp under build guards; with no data the
// table check fails and Begin() refuses to arm, so nothing fakes a played row.
// Tests: mod/tests/explus_core_test.cpp.

#include <cstdint>
#include <cstring>

namespace net::audio::explus::core {

    inline constexpr int kLayerCount   = 48;
    inline constexpr int kJointCount   = 11;
    inline constexpr int kVoxPoolCount = 8;
    inline constexpr int kQueueSlots   = 24;
    inline constexpr int kMaxCues      = 64;
    inline constexpr int kMixerVoices  = 32;    // ext MIX_VOICES
    inline constexpr int kVoiceGapMs   = 600;
    inline constexpr double kLateMs    = 250.0; // PCM rows
    inline constexpr double kVoxLateMs = 120.0;

    // ext EPL_* order (execplus_layers.py). src < 0 voice, 0 execplus.pcm,
    // >0 footsteps.pcm tier src-1 (1 sprint, 2 run, 3 walk).
    enum class Layer : uint8_t {
        kStepL = 0, kStepM, kStepH, kStepSweet, kScuff, kPivot,
        kWhArmS, kWhArmM, kWhArmL, kWhLeg, kWhBody, kWhGun,
        kClS, kClM, kClL,
        kGrTorso, kGrArm, kGrHead, kGrRelease,
        kStrugTorso, kStrugHead,
        kFallBodyM, kFallBodyH, kFallHead, kFallKnee, kFallHand,
        kHitBody, kSlide, kThump, kBone,
        kGearPack, kGearStrap, kGearHolster, kGearMelee,
        kWpnGripPistol, kWpnGripLong, kWpnLowerPistol, kWpnLowerLong,
        kBloodSplat, kBloodDrip,
        kVoxAttBreath, kVoxAttEffort, kVoxAttRelief,
        kVoxVicPain, kVoxVicStruggle, kVoxVicChoke, kVoxVicFall, kVoxVicWhimper
    };

    enum class Joint : uint8_t {
        kRoot = 0, kHead, kSpine, kPelvis, kLHand, kRHand,
        kLFoot, kRFoot, kLKnee, kRKnee, kWeapon
    };

    enum class Kind : uint8_t { kGun = 0, kMelee = 1 };

    // ext EPR_*, plus kSkipped for the row-level opt-outs (the ext counts those
    // as played) and kNoData when the guarded private tables are absent.
    enum class ResultCode : int8_t {
        kPlayed  = 1,
        kSkipped = 0,
        kVoice   = -1,  // skip[5]: all voices busy
        kPriority= -2,  // skip[1]: low-priority budget
        kLate    = -3,  // skip[2]
        kNoSource= -4,  // skip[4]: no body / position
        kMixer   = -5,  // skip[0]: mixer refused
        kVox     = -6,  // skip[3]: voice gate / refusal
        kNoData  = -8
    };

    struct LayerMeta { uint8_t prio; uint8_t base; int8_t src; };
    inline constexpr LayerMeta kLayerMeta[kLayerCount] = {
        {1,55,3},{1,62,2},{1,68,1},{2,45,0},{1,50,0},{2,45,0},
        {1,50,0},{1,58,0},{1,66,0},{1,62,0},{2,48,0},{1,55,0},
        {2,38,0},{2,44,0},{2,52,0},
        {0,55,0},{0,50,0},{0,52,0},{1,40,0},
        {1,42,0},{1,42,0},
        {0,60,0},{0,68,0},{0,56,0},{0,54,0},{1,46,0},
        {1,52,0},{1,44,0},{2,40,0},{2,36,0},
        {2,30,0},{2,32,0},{2,28,0},{2,30,0},
        {1,48,0},{1,48,0},{1,44,0},{1,44,0},
        {2,26,0},{2,22,0},
        {1,100,-1},{1,100,-1},{1,100,-1},
        {1,100,-1},{1,100,-1},{1,100,-1},{1,100,-1},{1,100,-1}
    };

    // The ext's skeleton names; WEAPON deliberately aliases the gun hand.
    inline const char* JointName(int j) noexcept
    {
        static constexpr const char* const kNames[kJointCount] = {
            "root", "headb", "spined", "pelvis", "l_palm", "r_palm",
            "l_ankle", "r_ankle", "l_knee", "r_knee", "r_palm"
        };
        return j >= 0 && j < kJointCount ? kNames[j] : nullptr;
    }

    // ext EP_VOX pools, in layer order. A refused event is marked bad by
    // ReportVoice() and not retried this session.
    inline constexpr const char* const kAttBreath[] = {
        "vox-eff-gen-meatshield-grab-struggle-combat", "vox-eff-gen-brth-fight-med",
        "vox-eff-gen-breath-fight-med", "vox-eff-gen-fight-give-grab"
    };
    inline constexpr const char* const kAttEffort[] = {
        "vox-eff-gen-fight-give-grab", "vox-eff-gen-fight-give-hvy-short",
        "vox-eff-gen-effort-sml", "vox-eff-gen-fight-give-sml"
    };
    inline constexpr const char* const kAttRelief[] = {
        "vox-eff-gen-breath-relief-sigh", "vox-eff-gen-breath-exhausted-recover",
        "vox-eff-gen-fight-give-exhstd", "vox-eff-gen-breath-fight-med"
    };
    inline constexpr const char* const kVicPain[] = {
        "vox-eff-gen-fight-take-face", "vox-eff-gen-fight-take-med-blnt",
        "vox-eff-gen-fight-take-med-blnt-gut", "vox-eff-gen-fight-take-sml-shrp"
    };
    inline constexpr const char* const kVicStruggle[] = {
        "vox-eff-gen-meatshield-grabbed-combat",
        "vox-eff-gen-meatshield-grabbed-struggle-combat",
        "vox-eff-gen-fight-struggle-med", "vox-eff-gen-choke-take-struggle"
    };
    inline constexpr const char* const kVicChoke[] = {
        "vox-eff-gen-death-gurgle-blood-sml", "vox-eff-gen-death-gurgle-rattle",
        "vox-eff-gen-death-gurgle-blood-lrg", "vox-eff-gen-cough-hvy"
    };
    inline constexpr const char* const kVicFall[] = {
        "vox-eff-gen-fight-take-hit-ground", "vox-eff-gen-fight-take-med-blnt-gut",
        "vox-eff-gen-fight-take-knockdown", "vox-eff-gen-fight-take-stunned"
    };
    inline constexpr const char* const kVicWhimper[] = {
        "vox-eff-gen-whimper-light", "vox-eff-gen-begs-for-life-stun",
        "vox-eff-gen-whimper-heavy", "vox-eff-gen-meatshield-grabbed-combat"
    };

    inline const char* const* VoxPool(int layer, int& count, int& vxe) noexcept
    {
        count = 4;
        switch (static_cast<Layer>(layer)) {
        case Layer::kVoxAttBreath:  return kAttBreath;
        case Layer::kVoxAttEffort:  vxe = 0; return kAttEffort;
        case Layer::kVoxAttRelief:  return kAttRelief;
        case Layer::kVoxVicPain:    vxe = 1; return kVicPain;
        case Layer::kVoxVicStruggle:vxe = 1; return kVicStruggle;
        case Layer::kVoxVicChoke:   vxe = 1; return kVicChoke;
        case Layer::kVoxVicFall:    vxe = 1; return kVicFall;
        case Layer::kVoxVicWhimper: vxe = 1; return kVicWhimper;
        default: count = 0; return nullptr;
        }
    }

    // ---- private-data view (filled only by guarded headers) ------------------

    struct WaveDesc { uint32_t off = 0; uint32_t n = 0; uint32_t rate = 0; };
    struct FootTier { uint16_t first = 0; uint16_t count = 0; };
    struct LayerSizing { uint8_t nv = 0; uint8_t mats = 0; uint16_t first = 0; };

    // EXPLUS_TL row. Rows of one (kind,pair) are contiguous and time-ordered;
    // the generator already ducked rows under the lead 2013 impacts.
    struct Row {
        uint8_t  kind  = 0;
        uint8_t  pair  = 0;
        uint8_t  layer = 0;
        uint8_t  who   = 0;
        uint8_t  joint = 0;
        uint8_t  gain  = 0;
        uint16_t ms    = 0;
    };

    struct DataView {
        const LayerSizing* sizing     = nullptr;
        int                layerCount = 0;
        const WaveDesc*    waves      = nullptr;
        int                waveCount  = 0;
        const WaveDesc*    footWaves  = nullptr;
        const Row*         rows       = nullptr;
        int                rowCount   = 0;
        const char* const* gunNames   = nullptr;
        int                gunCount   = 0;
        const char* const* meleeNames = nullptr;
        int                meleeCount = 0;
        const FootTier*    footTiers  = nullptr;
        int                footTierCount = 0;
        int                footWaveCount = 0;
        uint32_t           footRate   = 0;
        uint32_t           pcmSamples = 0;
        uint32_t           footPcmSamples = 0;
    };

    inline bool TablesOk(const DataView& d) noexcept
    {
        if (d.layerCount != kLayerCount || !d.sizing || !d.rows || d.rowCount <= 0)
            return false;
        if (!d.waves || d.waveCount <= 0 || d.pcmSamples == 0
            || !d.footWaves || d.footWaveCount <= 0 || d.footPcmSamples == 0)
            return false;
        for (int w = 0; w < d.waveCount; ++w)
            if (d.waves[w].rate < 8000
                || static_cast<uint64_t>(d.waves[w].off) + d.waves[w].n > d.pcmSamples)
                return false;
        for (int w = 0; w < d.footWaveCount; ++w)
            if (d.footWaves[w].rate < 8000
                || static_cast<uint64_t>(d.footWaves[w].off) + d.footWaves[w].n > d.footPcmSamples)
                return false;
        for (int l = 0; l < kLayerCount; ++l) {
            const int8_t src = kLayerMeta[l].src;
            if (src == 0) {
                const LayerSizing& s = d.sizing[l];
                if (!s.nv
                    || static_cast<int>(s.first) + static_cast<int>(s.nv) * s.mats > d.waveCount)
                    return false;
            }
            else if (src > 0 && src - 1 >= d.footTierCount)
                return false;
        }
        if (!d.footTiers || d.footTierCount <= 0 || d.footWaveCount <= 0
            || d.footRate < 8000 || d.footPcmSamples == 0)
            return false;
        for (int t = 0; t < d.footTierCount; ++t)
            if (!d.footTiers[t].count
                || static_cast<int>(d.footTiers[t].first) + d.footTiers[t].count > d.footWaveCount)
                return false;
        int prevKind = -1, prevPair = -1, prevMs = -100000;
        for (int i = 0; i < d.rowCount; ++i) {
            const Row& r = d.rows[i];
            if (r.layer >= kLayerCount || r.joint >= kJointCount || r.kind > 1)
                return false;
            if (static_cast<int>(r.kind) == prevKind && r.pair == prevPair
                && static_cast<int>(r.ms) < prevMs)
                return false;
            prevKind = r.kind;
            prevPair = r.pair;
            prevMs = r.ms;
        }
        return true;
    }

    // ---- timeline state ------------------------------------------------------

    struct QueueSlot {
        double   due = 0.0;
        uint8_t  layer = 0, who = 0, joint = 0, gain = 0;
        uint8_t  mat[2] = {0, 0};
        uint64_t ent[2] = {0, 0};
    };

    struct State {
        bool     on = false;
        int      kind = -1;
        int      pair = -1;
        int      lo = 0, hi = 0, next = 0;
        double   t0 = 0.0;
        int      played = 0;
        int      skip[6] = {0, 0, 0, 0, 0, 0}; // mixer, prio, late, vox, body, voice
        uint8_t  mat[2] = {0, 0};
        uint32_t waveRr[kLayerCount] = {};
        uint32_t voiceRr = 0;
        uint64_t attVoxMs = 0, vicVoxMs = 0;
        uint8_t  badVox[kLayerCount][4] = {};
        bool     vicDying = false;
        bool     havePos[2] = {false, false};
        float    pos[2][3] = {};
        QueueSlot q[kQueueSlots];
    };

    inline void Reset(State& s) noexcept { s = State{}; }

    inline bool Begin(State& s, const DataView& d, int kind, int pair,
                      const char* want, double t0, int attMat, int vicMat,
                      bool off, bool gated) noexcept
    {
        Reset(s);
        if (off || gated || (kind != 0 && kind != 1) || !TablesOk(d))
            return false;
        const char* have = kind == 0
            ? (pair >= 0 && pair < d.gunCount ? d.gunNames[pair] : nullptr)
            : (pair >= 0 && pair < d.meleeCount ? d.meleeNames[pair] : nullptr);
        if (!want || !have || std::strcmp(want, have))
            return false;
        int lo = -1, hi = 0;
        for (int i = 0; i < d.rowCount; ++i) {
            if (d.rows[i].kind == static_cast<uint8_t>(kind) && d.rows[i].pair == pair) {
                if (!hi)
                    lo = i;
                hi = i + 1;
            }
            else if (hi)
                break;
        }
        if (lo < 0 || hi <= lo)
            return false;
        s.on = true;
        s.kind = kind;
        s.pair = pair;
        s.lo = lo;
        s.hi = hi;
        s.next = lo;
        s.t0 = t0;
        s.mat[0] = attMat == 1 ? 1 : 0;
        s.mat[1] = vicMat == 1 ? 1 : 0;
        return true;
    }

    inline bool Matches(const State& s, int kind, int pair) noexcept
    {
        return s.on && s.kind == kind && s.pair == pair;
    }

    inline void Cut(State& s) noexcept { Reset(s); }

    // ---- emitted work --------------------------------------------------------

    struct Cue {
        enum class Kind : uint8_t { kPcm, kVox } kind = Kind::kPcm;
        int      layer = 0;
        int      who   = 0;
        int      joint = 0;
        int      material = 0;
        float    gain = 0.0f;
        double   late = 0.0;
        uint64_t ent = 0;
        bool     havePos = false;
        float    pos[3] = {0, 0, 0};
        int      wave = -1;        // PCM: index into DataView::waves or ::footWaves
        bool     foot = false;     // true -> footWaves / the footsteps PCM blob
        uint32_t rate = 0;
        int      startFrame = 0;   // ext's lateness seek into the wave
        int      samples = 0;
        int      eventIndex = -1;  // VOX: index into the layer's pool
        int      vxe = 0;          // 0 give/effort, 1 pain
    };

    struct Result {
        Cue  cues[kMaxCues];
        int  count = 0;
    };

    inline void AddCue(Result& r, const Cue& c) noexcept
    {
        if (r.count < kMaxCues)
            r.cues[r.count++] = c;
    }

    struct TickInput {
        double   nowMs = 0.0;      // wall clock for wave rotation / voice gates
        double   ems   = 0.0;
        double   level = 1.0;      // g_ex.gain (distance level)
        int      activeVoices = 0; // ext ep_voices_busy(); owner supplies it
        uint64_t attEnt = 0, vicEnt = 0;
        bool     haveAttPos = false, haveVicPos = false;
        float    attPos[3] = {0, 0, 0}, vicPos[3] = {0, 0, 0};
        bool     off = false;
        bool     handSlaps = false, fistRub = false, stompClaps = false;
        bool     shivActive = false, vicLine = false, ebCovers = false;
        bool     voxClash = false;
        float    lead = 0.45f, plus = 0.70f;
    };

    inline void Record(State& s, ResultCode code, int& played) noexcept
    {
        if (code == ResultCode::kPlayed || code == ResultCode::kSkipped) {
            ++s.played;
            ++played;
        }
        else if (code == ResultCode::kMixer) ++s.skip[0];
        else if (code == ResultCode::kPriority) ++s.skip[1];
        else if (code == ResultCode::kLate) ++s.skip[2];
        else if (code == ResultCode::kVox) ++s.skip[3];
        else if (code == ResultCode::kNoSource) ++s.skip[4];
        else if (code == ResultCode::kVoice) ++s.skip[5];
    }

    inline int PickVoice(State& s, int layer, int who, uint64_t nowMs) noexcept
    {
        if (who == 0 ? (s.attVoxMs && nowMs - s.attVoxMs < kVoiceGapMs)
                     : (s.vicVoxMs && nowMs - s.vicVoxMs < kVoiceGapMs))
            return -1;
        if (who == 1 && s.vicDying && layer != static_cast<int>(Layer::kVoxVicChoke))
            return -1;
        for (int t = 0; t < 4; ++t) {
            const int k = static_cast<int>((s.voiceRr + static_cast<uint32_t>(t)) % 4u);
            if (!s.badVox[layer][k])
                return k;
        }
        return -1;
    }

    // A pool take the engine already refused this session (ext g_ep_vox_bad).
    inline bool BadVoice(const State& s, int layer, int k) noexcept
    {
        return layer >= 0 && layer < kLayerCount && k >= 0 && k < 4
            && s.badVox[layer][k] != 0;
    }

    // The owner calls this after say(). A refusal marks that pool take bad for
    // the session; an acceptance advances the shared rotation and the 600 ms
    // speaker gate, exactly like ext ep_vox. nowMs is the wall clock, never the
    // cue's lateness.
    inline void ReportVoice(State& s, int layer, int who, int eventIndex,
                            bool accepted, uint64_t nowMs) noexcept
    {
        if (layer < 0 || layer >= kLayerCount || eventIndex < 0 || eventIndex >= 4)
            return;
        if (!accepted) {
            s.badVox[layer][eventIndex] = 1;
            return;
        }
        s.voiceRr += 1u + static_cast<uint32_t>(nowMs & 3u);
        if (who == 0)
            s.attVoxMs = nowMs;
        else
            s.vicVoxMs = nowMs;
    }

    // A PCM voice the mixer refused is the ext's EPR_MIXER: undo the optimistic
    // played count and move it to skip[0].
    inline void ReportMixer(State& s, bool accepted) noexcept
    {
        if (!accepted && s.played > 0) {
            --s.played;
            ++s.skip[0];
        }
    }

    struct FireInput {
        int      layer = 0;
        int      who = 0;
        int      joint = 0;
        float    gain01 = 0.0f;
        double   late = 0.0;
        uint64_t ent = 0;
        bool     havePos = false;
        float    pos[3] = {0, 0, 0};
        int      material = 0;
        float    level = 1.0;
        double   nowMs = 0.0;
        int      activeVoices = 0;
        bool     ebCovers = false;
        float    lead = 0.45f, plus = 0.70f;
    };

    inline ResultCode Fire(State& s, const DataView& d, const FireInput& in, Result& out) noexcept
    {
        if (in.late > kLateMs)
            return ResultCode::kLate;
        const double late = in.late < 0.0 ? 0.0 : in.late;
        if (in.layer < 0 || in.layer >= kLayerCount)
            return ResultCode::kNoData;
        const LayerMeta& meta = kLayerMeta[in.layer];

        if (meta.src < 0) {
            if (!in.ent || in.ent == ~0ULL)
                return ResultCode::kNoSource;
            if (late > kVoxLateMs)
                return ResultCode::kLate;
            if (in.who == 1 && in.ebCovers)
                return ResultCode::kVox;
            if (in.who == 1 && s.vicDying
                && in.layer != static_cast<int>(Layer::kVoxVicChoke))
                return ResultCode::kVox;
            if (in.who == 0 ? (s.attVoxMs && in.nowMs - s.attVoxMs < kVoiceGapMs)
                            : (s.vicVoxMs && in.nowMs - s.vicVoxMs < kVoiceGapMs))
                return ResultCode::kVox;
            const int k = PickVoice(s, in.layer, in.who, static_cast<uint64_t>(in.nowMs));
            if (k < 0)
                return ResultCode::kVox;
            int count = 0;
            int vxe = 0;
            const char* const* pool = VoxPool(in.layer, count, vxe);
            if (!pool || count <= k || !pool[k])
                return ResultCode::kVox;
            Cue c;
            c.kind = Cue::Kind::kVox;
            c.layer = in.layer;
            c.who = in.who;
            c.joint = in.joint;
            c.material = in.material;
            c.gain = in.gain01;
            c.late = late;
            c.ent = in.ent;
            c.eventIndex = k;
            c.vxe = vxe;
            AddCue(out, c);
            return ResultCode::kPlayed;
        }

        const int busy = in.activeVoices;
        if (busy >= 27)
            return ResultCode::kVoice;
        if ((meta.prio >= 1 && busy >= 22) || (meta.prio >= 2 && busy >= 16))
            return ResultCode::kPriority;
        if ((!in.ent || in.ent == ~0ULL) && !in.havePos)
            return ResultCode::kNoSource;

        int wave = -1;
        bool foot = false;
        uint32_t rate = 0;
        uint32_t samples = 0;
        const uint32_t spin = static_cast<uint32_t>(in.nowMs) >> 3;
        if (meta.src > 0) {
            const int tier = meta.src - 1;
            if (tier < 0 || tier >= d.footTierCount || !d.footTiers)
                return ResultCode::kNoData;
            const FootTier& f = d.footTiers[tier];
            if (!f.count)
                return ResultCode::kNoData;
            wave = f.first + static_cast<int>((s.waveRr[in.layer]++ * 5u + spin) % f.count);
            if (wave < 0 || wave >= d.footWaveCount || !d.footWaves)
                return ResultCode::kNoData;
            rate = d.footRate;
            samples = d.footWaves[wave].n;
            foot = true;
        }
        else {
            const LayerSizing& sz = d.sizing[in.layer];
            if (!sz.nv || !d.waves)
                return ResultCode::kNoData;
            const int m = sz.mats > 1 ? in.material % sz.mats : 0;
            const int v = static_cast<int>((s.waveRr[in.layer]++ * 3u + spin) % sz.nv);
            wave = sz.first + m * sz.nv + v;
            if (wave < 0 || wave >= d.waveCount)
                return ResultCode::kNoData;
            rate = d.waves[wave].rate;
            samples = d.waves[wave].n;
        }
        const int skip = static_cast<int>(late * static_cast<double>(rate) / 1000.0);
        if (skip > static_cast<int>(samples) - 256 || samples == 0)
            return ResultCode::kLate;

        Cue c;
        c.kind = Cue::Kind::kPcm;
        c.layer = in.layer;
        c.foot = foot;
        c.who = in.who;
        c.joint = in.joint;
        c.material = in.material;
        c.gain = in.gain01 * static_cast<float>(meta.base) / 100.0f
               * in.lead * in.plus * in.level;
        c.late = late;
        c.ent = in.ent == ~0ULL ? 0 : in.ent;
        c.havePos = in.havePos;
        for (int i = 0; i < 3; ++i)
            c.pos[i] = in.pos[i];
        c.wave = wave;
        c.rate = rate;
        c.startFrame = skip;
        c.samples = static_cast<int>(samples) - skip;
        AddCue(out, c);
        return ResultCode::kPlayed;
    }

    inline bool FistRubLayer(int layer) noexcept
    {
        return (layer >= static_cast<int>(Layer::kScuff)
                && layer <= static_cast<int>(Layer::kClL))
            || (layer >= static_cast<int>(Layer::kGrRelease)
                && layer <= static_cast<int>(Layer::kGearStrap)
                && layer != static_cast<int>(Layer::kStrugTorso)
                && layer != static_cast<int>(Layer::kStrugHead))
            || layer == static_cast<int>(Layer::kGearHolster)
            || layer == static_cast<int>(Layer::kGearMelee);
    }

    inline ResultCode RowCode(State& s, const DataView& d, const TickInput& in,
                              int ri, Result& out) noexcept
    {
        const Row& r = d.rows[ri];
        if (s.kind == 1 && in.voxClash && kLayerMeta[r.layer].src < 0)
            return ResultCode::kVox;
        if (r.layer == static_cast<int>(Layer::kFallHand) && !in.handSlaps)
            return ResultCode::kSkipped;
        if (s.kind == 1 && (s.pair == 0 || s.pair == 1) && r.ms < 1300
            && FistRubLayer(r.layer) && !in.fistRub)
            return ResultCode::kSkipped;
        if (s.kind == 1 && (s.pair == 2 || s.pair == 3)
            && (r.layer == static_cast<int>(Layer::kFallHead)
                || r.layer == static_cast<int>(Layer::kBone))
            && !in.stompClaps)
            return ResultCode::kSkipped;

        float g01 = static_cast<float>(r.gain) / 255.0f;
        if (s.kind == 1 && in.vicLine)
            g01 *= 0.5f;
        if (s.kind == 1 && r.layer >= static_cast<int>(Layer::kWhArmS)
            && r.layer <= static_cast<int>(Layer::kWhArmL) && in.shivActive)
            g01 *= 0.30f;

        FireInput f;
        f.layer = r.layer;
        f.who = r.who;
        f.joint = r.joint;
        f.gain01 = g01;
        f.late = in.ems - static_cast<double>(r.ms);
        f.ent = r.who ? in.vicEnt : in.attEnt;
        f.havePos = r.who ? in.haveVicPos : in.haveAttPos;
        for (int i = 0; i < 3; ++i)
            f.pos[i] = r.who ? in.vicPos[i] : in.attPos[i];
        f.material = r.who ? s.mat[1] : s.mat[0];
        f.level = in.level;
        f.nowMs = in.nowMs;
        f.activeVoices = in.activeVoices;
        f.ebCovers = in.ebCovers;
        f.lead = in.lead;
        f.plus = in.plus;
        return Fire(s, d, f, out);
    }

    inline int Tick(State& s, const DataView& d, const TickInput& in, Result& out) noexcept
    {
        if (!s.on || in.off)
            return 0;
        int played = 0;
        while (s.next < s.hi
               && static_cast<double>(d.rows[s.next].ms) <= in.ems) {
            Record(s, RowCode(s, d, in, s.next, out), played);
            ++s.next;
        }
        return played;
    }

    // ---- recipes (ctxdown / finishers / downed melee / interrogation foley) ---

    struct RecipeRow { int16_t dly; uint8_t layer, who, joint, gain; };
    struct Recipe { const RecipeRow* rows; int count; };

#define TO1_EP_ROWS(name, ...) \
    static constexpr RecipeRow name[] = { __VA_ARGS__ }
#define TO1_EP_R(l,w,j,g) { 0, static_cast<uint8_t>(Layer::l), (w), \
                            static_cast<uint8_t>(Joint::k##j), (g) }
#define TO1_EP_D(d,l,w,j,g) { (d), static_cast<uint8_t>(Layer::l), (w), \
                              static_cast<uint8_t>(Joint::k##j), (g) }

    TO1_EP_ROWS(kCtxGrab,  TO1_EP_R(kGrTorso,0,RHand,75), TO1_EP_D(20,kClM,1,Spine,60),
                TO1_EP_R(kStepM,0,RFoot,55), TO1_EP_D(60,kGearPack,0,Spine,45));
    TO1_EP_ROWS(kCtxGrunt, TO1_EP_R(kStepM,0,LFoot,50), TO1_EP_D(100,kClM,0,Spine,45));
    TO1_EP_ROWS(kCtxHit,   TO1_EP_R(kClL,0,Spine,55), TO1_EP_D(30,kClM,1,Spine,50),
                TO1_EP_R(kGearPack,0,Spine,40));
    TO1_EP_ROWS(kCtxHeavy, TO1_EP_R(kClL,0,Spine,70), TO1_EP_D(40,kGearPack,0,Spine,50),
                TO1_EP_D(50,kClL,1,Spine,60));
    TO1_EP_ROWS(kCtxHeavyW,TO1_EP_R(kThump,1,Spine,55), TO1_EP_D(5,kBone,1,Head,55));
    TO1_EP_ROWS(kCtxThud,  TO1_EP_R(kFallHead,1,Head,70), TO1_EP_D(10,kClL,1,Spine,60),
                TO1_EP_D(30,kThump,1,Spine,50), TO1_EP_D(40,kFallHand,1,RHand,50));
    TO1_EP_ROWS(kCtxFall,  TO1_EP_R(kFallBodyH,1,Pelvis,80), TO1_EP_R(kThump,1,Pelvis,65),
                TO1_EP_D(20,kFallHand,1,LHand,50), TO1_EP_D(50,kGearPack,1,Spine,60),
                TO1_EP_D(90,kFallHead,1,Head,45), TO1_EP_D(140,kClL,1,Spine,60),
                TO1_EP_D(220,kSlide,1,Pelvis,45));
    TO1_EP_ROWS(kCtxWhoosh,TO1_EP_R(kWhBody,0,Pelvis,55), TO1_EP_D(40,kClL,0,Spine,55),
                TO1_EP_D(70,kStepM,0,LFoot,60));
    TO1_EP_ROWS(kCtxKick,  TO1_EP_R(kHitBody,1,Spine,80), TO1_EP_R(kStepH,0,RFoot,80),
                TO1_EP_D(20,kClL,0,Spine,60));

    TO1_EP_ROWS(kFinGrab,  TO1_EP_R(kGrArm,0,RHand,60), TO1_EP_D(10,kClM,0,Spine,50),
                TO1_EP_R(kStepL,0,LFoot,45));
    TO1_EP_ROWS(kFinPunch, TO1_EP_R(kClM,0,Spine,50), TO1_EP_R(kStepM,0,RFoot,50),
                TO1_EP_D(30,kGearPack,0,Spine,35));
    TO1_EP_ROWS(kFinHeavy, TO1_EP_R(kClL,0,Spine,65), TO1_EP_R(kStepH,0,RFoot,60),
                TO1_EP_D(30,kGearPack,0,Spine,45));
    TO1_EP_ROWS(kFinKick,  TO1_EP_R(kStepH,0,LFoot,70), TO1_EP_R(kClL,0,Spine,60),
                TO1_EP_D(30,kGearPack,0,Spine,40));
    TO1_EP_ROWS(kFinSwing, TO1_EP_R(kWhBody,0,Pelvis,50), TO1_EP_R(kClL,0,Spine,55),
                TO1_EP_D(60,kStepM,0,LFoot,50), TO1_EP_D(30,kGearMelee,0,Spine,45));
    TO1_EP_ROWS(kFinBlow,  TO1_EP_R(kClL,0,Spine,65), TO1_EP_R(kStepH,0,RFoot,55),
                TO1_EP_D(20,kGearMelee,0,Spine,50));

    TO1_EP_ROWS(kDownEffort,TO1_EP_R(kClM,0,Spine,50), TO1_EP_R(kStepM,0,LFoot,55),
                TO1_EP_D(40,kGearPack,0,Spine,40));
    TO1_EP_ROWS(kDownSwing, TO1_EP_R(kWhBody,0,Pelvis,50), TO1_EP_D(30,kClL,0,Spine,55));
    TO1_EP_ROWS(kDownKick,  TO1_EP_R(kHitBody,1,Spine,75), TO1_EP_R(kStepH,0,RFoot,70),
                TO1_EP_D(10,kClL,0,Spine,55), TO1_EP_D(20,kClM,1,Spine,50));
    TO1_EP_ROWS(kDownBlow,  TO1_EP_R(kHitBody,1,Spine,70), TO1_EP_R(kStepH,0,RFoot,65),
                TO1_EP_D(5,kBone,1,Head,40), TO1_EP_D(10,kClL,0,Spine,55),
                TO1_EP_D(20,kClM,1,Spine,50));
    TO1_EP_ROWS(kDownMoan,  TO1_EP_R(kFallHand,1,RHand,45), TO1_EP_D(30,kClM,1,Spine,45));

    TO1_EP_ROWS(kIntFist0, TO1_EP_R(kStrugTorso,1,Spine,62), TO1_EP_D(50,kClM,0,Spine,46),
                TO1_EP_D(120,kClS,1,Spine,42));
    TO1_EP_ROWS(kIntFist1, TO1_EP_R(kStrugHead,1,Head,56), TO1_EP_D(40,kClM,1,Spine,44),
                TO1_EP_D(90,kGearPack,0,Spine,38));
    TO1_EP_ROWS(kIntFist2, TO1_EP_R(kClL,1,Spine,54), TO1_EP_D(20,kGrArm,0,RHand,40),
                TO1_EP_D(80,kSlide,1,Pelvis,34));
    TO1_EP_ROWS(kIntShiv0, TO1_EP_R(kStrugTorso,1,Spine,60), TO1_EP_D(50,kClM,0,Spine,46),
                TO1_EP_D(110,kScuff,1,LFoot,42));
    TO1_EP_ROWS(kIntShiv1, TO1_EP_R(kStrugHead,1,Head,54), TO1_EP_D(40,kClM,1,Spine,44),
                TO1_EP_D(100,kGearPack,0,Spine,36));
    TO1_EP_ROWS(kIntShiv2, TO1_EP_R(kClL,1,Spine,52), TO1_EP_D(20,kGrArm,0,RHand,40),
                TO1_EP_D(70,kStepL,0,RFoot,40), TO1_EP_D(130,kScuff,1,RFoot,40));

#undef TO1_EP_D
#undef TO1_EP_R
#undef TO1_EP_ROWS

    inline Recipe ContextRecipe(int act, int style) noexcept
    {
        switch (act) {
        case 0:  return { kCtxGrab,   static_cast<int>(sizeof kCtxGrab / sizeof kCtxGrab[0]) };
        case 1:  return { kCtxGrunt,  static_cast<int>(sizeof kCtxGrunt / sizeof kCtxGrunt[0]) };
        case 2:  return { kCtxHit,    static_cast<int>(sizeof kCtxHit / sizeof kCtxHit[0]) };
        case 3:  return { kCtxHeavy,  static_cast<int>(sizeof kCtxHeavy / sizeof kCtxHeavy[0]) };
        case 4:  return { kCtxThud,   static_cast<int>(sizeof kCtxThud / sizeof kCtxThud[0]) };
        case 5:  return { kCtxFall,   static_cast<int>(sizeof kCtxFall / sizeof kCtxFall[0]) };
        case 6:  return { kCtxWhoosh, static_cast<int>(sizeof kCtxWhoosh / sizeof kCtxWhoosh[0]) };
        case 9:  return { kCtxKick,   static_cast<int>(sizeof kCtxKick / sizeof kCtxKick[0]) };
        default: (void)style; return { nullptr, 0 };
        }
    }

    inline Recipe HeavyContextRecipe(int style) noexcept
    {
        (void)style;
        return { kCtxHeavy, static_cast<int>(sizeof kCtxHeavy / sizeof kCtxHeavy[0]) };
    }

    inline Recipe FinisherRecipe(int label) noexcept
    {
        switch (label) {
        case 0:  return { kFinGrab,  static_cast<int>(sizeof kFinGrab / sizeof kFinGrab[0]) };
        case 1:  return { kFinPunch, static_cast<int>(sizeof kFinPunch / sizeof kFinPunch[0]) };
        case 2:  return { kFinHeavy, static_cast<int>(sizeof kFinHeavy / sizeof kFinHeavy[0]) };
        case 3: case 4:
                 return { kFinKick,  static_cast<int>(sizeof kFinKick / sizeof kFinKick[0]) };
        case 5: case 6:
                 return { kFinSwing, static_cast<int>(sizeof kFinSwing / sizeof kFinSwing[0]) };
        case 7: case 8: case 9:
                 return { kFinBlow,  static_cast<int>(sizeof kFinBlow / sizeof kFinBlow[0]) };
        default: return { nullptr, 0 };
        }
    }

    inline Recipe DownedRecipe(int kind, int cls) noexcept
    {
        switch (kind) {
        case 0: return { kDownEffort, static_cast<int>(sizeof kDownEffort / sizeof kDownEffort[0]) };
        case 1: return { kDownSwing,  static_cast<int>(sizeof kDownSwing / sizeof kDownSwing[0]) };
        case 2: return cls
            ? Recipe{ kDownBlow, static_cast<int>(sizeof kDownBlow / sizeof kDownBlow[0]) }
            : Recipe{ kDownKick, static_cast<int>(sizeof kDownKick / sizeof kDownKick[0]) };
        default:return { kDownMoan,  static_cast<int>(sizeof kDownMoan / sizeof kDownMoan[0]) };
        }
    }

    inline Recipe IntFoleyRecipe(int k, bool fist) noexcept
    {
        const int i = ((k % 3) + 3) % 3;
        if (fist) {
            static constexpr Recipe kFist[3] = {
                { kIntFist0, static_cast<int>(sizeof kIntFist0 / sizeof kIntFist0[0]) },
                { kIntFist1, static_cast<int>(sizeof kIntFist1 / sizeof kIntFist1[0]) },
                { kIntFist2, static_cast<int>(sizeof kIntFist2 / sizeof kIntFist2[0]) }
            };
            return kFist[i];
        }
        static constexpr Recipe kShiv[3] = {
            { kIntShiv0, static_cast<int>(sizeof kIntShiv0 / sizeof kIntShiv0[0]) },
            { kIntShiv1, static_cast<int>(sizeof kIntShiv1 / sizeof kIntShiv1[0]) },
            { kIntShiv2, static_cast<int>(sizeof kIntShiv2 / sizeof kIntShiv2[0]) }
        };
        return kShiv[i];
    }

    struct RecipeInput {
        double nowMs = 0.0;
        double ems   = 0.0;
        double level = 1.0;
        int activeVoices = 0;
        bool off = false;
        bool shivActive = false, vicLine = false, ebCovers = false, voxClash = false;
        float lead = 0.45f, plus = 0.70f;
    };

    inline int QueueRecipe(State& s, const DataView& d, const Recipe& rec,
                           uint64_t att, uint64_t vic, int matA, int matV,
                           const RecipeInput& in, Result& out) noexcept
    {
        if (rec.rows && rec.count <= 0)
            return 0;
        int played = 0;
        for (int i = 0; i < rec.count; ++i) {
            const RecipeRow& r = rec.rows[i];
            const int who = r.who ? 1 : 0;
            const uint64_t ent = who ? vic : att;
            if (!ent || ent == ~0ULL)
                continue;
            const int mat = who ? matV : matA;
            if (r.dly <= 1) {
                FireInput f;
                f.layer = r.layer;
                f.who = who;
                f.joint = r.joint;
                f.gain01 = static_cast<float>(r.gain) / 100.0f;
                f.late = 0.0;
                f.ent = ent;
                f.material = mat;
                f.level = 1.0;
                f.nowMs = in.nowMs;
                f.activeVoices = in.activeVoices;
                f.ebCovers = in.ebCovers;
                f.lead = in.lead;
                f.plus = in.plus;
                Record(s, Fire(s, d, f, out), played);
                continue;
            }
            int slot = -1;
            for (int q = 0; q < kQueueSlots; ++q)
                if (s.q[q].due == 0.0) { slot = q; break; }
            if (slot < 0)
                continue;                    // ext drops when the 24 slots are full
            QueueSlot& q = s.q[slot];
            q.due = in.nowMs + r.dly;
            q.layer = r.layer;
            q.who = static_cast<uint8_t>(who);
            q.joint = r.joint;
            q.gain = r.gain;
            q.mat[0] = static_cast<uint8_t>(matA == 1 ? 1 : 0);
            q.mat[1] = static_cast<uint8_t>(matV == 1 ? 1 : 0);
            q.ent[0] = att;
            q.ent[1] = vic;
        }
        (void)d;
        return played;
    }

    inline int TickQueue(State& s, const DataView& d, double nowMs,
                         const RecipeInput& in, Result& out) noexcept
    {
        if (in.off)
            return 0;
        int played = 0;
        for (int i = 0; i < kQueueSlots; ++i) {
            QueueSlot& q = s.q[i];
            if (q.due == 0.0 || nowMs < q.due)
                continue;
            const double late = nowMs - q.due;
            const int who = q.who ? 1 : 0;
            const uint64_t ent = q.ent[who];
            q.due = 0.0;
            FireInput f;
            f.layer = q.layer;
            f.who = who;
            f.joint = q.joint;
            f.gain01 = static_cast<float>(q.gain) / 100.0f;
            f.late = late;
            f.ent = ent;
            f.material = q.mat[who];
            f.level = 1.0;
            f.nowMs = nowMs;
            f.activeVoices = in.activeVoices;
            f.ebCovers = in.ebCovers;
            f.lead = in.lead;
            f.plus = in.plus;
            Record(s, Fire(s, d, f, out), played);
        }
        return played;
    }

} // namespace net::audio::explus::core
