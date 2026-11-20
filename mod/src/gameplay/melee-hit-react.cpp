// ported from tools/ext/melee_hitreact.c (the timing / state core)

#include "melee-hit-react.hpp"

#include <algorithm>
#include <cstring>

namespace gameplay::melee_hit_react {

    namespace {

        Sink* g_sink = nullptr;
        bool  g_pain = true;
        bool  g_punch = true;
        bool  g_predict = true;

        // ---- victim (our own body) --------------------------------------------------------
        struct Victim {
            bool     active = false;
            uint64_t sid = 0;
            uint64_t startMs = 0;
            uint16_t cutMs = 0;
            int      strength = 0;
            uint64_t lastStartMs = 0;   // ext g_mhr.last_start_ms (graze window)
            uint64_t lastSid = 0;       // ext g_mhr.last_sid (no two in a row)
        } g_v;

        // ---- attacker: the one provisional lock (ext g_mhr.atk) ---------------------------
        struct Atk {
            uint8_t  slot = 255;
            uint64_t ms = 0;
            int      str = 0;
        } g_atk;
        uint64_t g_lockUntil = 0;       // ext MpaceNoteVictim's held-swing start

        // ---- predicted puppet reaction (ext g_mhr.pr[slot]) --------------------------------
        struct Pred {
            uint64_t due = 0;           // ext pr[].due (fire at contact + 50 ms)
            uint64_t sid = 0;
            uint64_t played = 0;
            uint64_t last = 0;          // ext pr[].last (graze window on our copy)
            uint16_t cut = 0;
            int      str = 0;
            bool     sharp = false;
            bool     pain = false;
        } g_pred[kMaxPeers];

        // ---- the clip we forced on each peer's puppet (ext g_mhr.pd[slot]) -----------------
        struct Puppet {
            uint64_t sid = 0;
            uint64_t end = 0;
        } g_puppet[kMaxPeers];

        uint8_t PainBits(int str)
        {
            if (!g_pain)
                return 0;
            const uint8_t s = static_cast<uint8_t>(std::clamp(str, 0, 2));
            return static_cast<uint8_t>(kFPain | (s << 3));
            // `sharp` is OR-ed in by the caller that knows the blow's class.
        }

        void SendReaction(uint64_t sid, uint16_t cut, int str, bool sharp, bool punch)
        {
            if (!g_sink)
                return;
            uint8_t b[kMessageSize] = {};
            b[0] = kFReaction;
            b[0] |= PainBits(str);
            if (sharp)
                b[0] |= 0x20;
            if (punch && g_punch)
                b[0] |= kFPunch;
            std::memcpy(b + 1, &sid, 8);
            std::memcpy(b + 9, &cut, 2);
            g_sink->SendMessage(b);
        }

        void SendNoReaction(int landed, bool punch)
        {
            if (!g_sink)
                return;
            uint8_t b[kMessageSize] = {};
            b[0] = landed > 0 ? kFLanded : 0;
            if (landed > 0 && punch && g_punch)
                b[0] |= kFPunch;
            if (landed >= 2)
                b[0] |= PainBits(1);
            g_sink->SendMessage(b);
        }

        void StopPuppet(uint8_t slot)
        {
            if (g_sink && g_puppet[slot].sid)
                g_sink->StopPuppetReaction(slot, g_puppet[slot].sid, 0.45f); // MHR_BLEND_OUT
            g_puppet[slot] = Puppet{};
        }

        void ArmPrediction(int slot, const Blow& blow, int str, bool twoHanded)
        {
            if (!g_sink || !g_predict || slot < 0 || slot >= kMaxPeers)
                return;
            if (g_pred[slot].last && g_sink->NowMs() - g_pred[slot].last < kGrazeMs)
                return;                 // the victim's grazing window: no speculative clip
            uint64_t sid = 0;
            uint16_t cut = kCutMs[std::clamp(str, 0, 2)];
            int used = str;
            if (!g_sink->PickClip(blow, str, twoHanded, g_pred[slot].sid,
                                  &sid, &cut, &used) || !sid)
                return;
            Pred& p = g_pred[slot];
            p.sid = sid;
            p.cut = cut;
            p.str = std::clamp(used, 0, 2);
            p.sharp = blow.sharp;
            p.played = 0;
            p.due = g_sink->NowMs() + kPredictGrace;
        }

    } // namespace

    void SetSink(Sink* sink) { g_sink = sink; }
    void SetPainEnabled(bool enabled) noexcept { g_pain = enabled; }
    void SetPunchEnabled(bool enabled) noexcept { g_punch = enabled; }
    void SetPredictionEnabled(bool enabled) noexcept { g_predict = enabled; }

    Strength DeriveStrength(BlowClass cls, float damage, bool* twoHanded)
    {
        const bool weapon = cls != kFist;
        const bool two = cls == kTwoHanded;
        if (twoHanded)
            *twoHanded = two;
        int str = !weapon ? 0 : two ? 2 : 1;
        if (!weapon && damage >= 28.0f)
            str = 1;                    // an upgraded / hard fist blow
        else if (weapon && !two && damage >= 45.0f)
            str = 2;
        return static_cast<Strength>(std::clamp(str, 0, 2));
    }

    // ---- victim --------------------------------------------------------------------------

    bool OnBlowTaken(int slot, const Blow& blow, const VictimGates& gates)
    {
        if (!g_sink)
            return false;
        const uint64_t now = g_sink->NowMs();
        bool two = false;
        const int raw = static_cast<int>(DeriveStrength(blow.cls, blow.damage, &two));

        // ext MhrVictimStart's skip chain. A running reaction is replaced only by a plain duel
        // blow or once its control has come back (past the cut); the graze window is bypassed
        // by a plain duel blow.
        const bool pastCut = g_v.active && now >= g_v.startMs + g_v.cutMs;
        const bool replace = g_v.active && (gates.plainDuelBlow || pastCut);
        if (g_v.active && !replace)
            return false;
        if (!gates.plainDuelBlow && g_v.lastStartMs
            && now - g_v.lastStartMs < kGrazeMs)
            return false;
        if (gates.busy || gates.crouched || gates.duelOwnsReaction)
            return false;

        uint64_t sid = 0;
        uint16_t cut = kCutMs[raw];
        int used = raw;
        if (!g_sink->PickClip(blow, raw, two, g_v.lastSid, &sid, &cut, &used) || !sid)
            return false;               // no clip loaded: the attacker's lock is released by
                                        // the caller via NoteNoReaction()

        // A replacement ends the old local clip first.
        if (g_v.active && g_sink)
            g_sink->StopLocalReaction(g_v.sid, 0.45f);

        g_v.active = true;
        g_v.sid = sid;
        g_v.startMs = now;
        g_v.cutMs = cut;
        g_v.strength = std::clamp(used, 0, 2);
        g_v.lastStartMs = now;
        g_v.lastSid = sid;

        g_sink->PlayLocalReaction(sid, cut, g_v.strength);
        if (g_pain)
            g_sink->PainSound(255, g_v.strength, blow.sharp);
        if (blow.fistSfx && g_punch)
            g_sink->Punch(255, g_v.strength);
        SendReaction(sid, cut, g_v.strength, blow.sharp, blow.fistSfx);
        return true;
    }

    void EndReactionEarly(const char* why)
    {
        if (!g_v.active)
            return;
        if (g_sink) {
            g_sink->StopLocalReaction(g_v.sid, 0.45f);
            if (why)
                g_sink->Log(why);
        }
        // ext MhrEnd: flags 0x80 on the ps25 full-reaction path tells the other screens to
        // blend their copy out at the exit rather than waiting for the authored end.
        uint8_t b[kMessageSize] = {};
        b[0] = kFLeft;
        std::memcpy(b + 1, &g_v.sid, 8);
        if (g_sink)
            g_sink->SendMessage(b);
        g_v.active = false;
        g_v.sid = 0;
    }

    void NoteNoReaction(int landed, bool fistSfx)
    {
        SendNoReaction(landed, fistSfx);
        if (landed >= 2 && g_pain && g_sink)
            g_sink->PainSound(255, 1, false);
    }

    void VictimTick()
    {
        if (!g_v.active || !g_sink)
            return;
        if (g_sink->NowMs() >= g_v.startMs + g_v.cutMs) {
            // The clip's impact part is over: control returns. The authored clip may keep
            // playing to its exit on the other screens (that timing is gated in ExitMsOf).
            g_v.active = false;
        }
    }

    bool ReactionActive() noexcept { return g_v.active; }
    uint64_t ReactionSid() noexcept { return g_v.sid; }

    // ---- attacker -------------------------------------------------------------------------

    void OnBlowDealt(int slot, const Blow& blow, bool heavy, bool dodgeImmune)
    {
        if (!g_sink || heavy || slot < 0 || slot >= kMaxPeers)
            return;
        bool two = false;
        const int str = static_cast<int>(DeriveStrength(blow.cls, blow.damage, &two));

        // ext MhrAtkHit: hold our next swing until the impact part of the victim's reaction
        // is over. The victim's message corrects (or releases) this provisional time.
        g_atk.slot = static_cast<uint8_t>(slot);
        g_atk.ms = g_sink->NowMs();
        g_atk.str = str;
        const uint16_t cut = kCutMs[str] < kAtkMaxMs ? kCutMs[str] : kAtkMaxMs;
        g_lockUntil = g_atk.ms + cut;

        if (!dodgeImmune)
            ArmPrediction(slot, blow, str, two);
    }

    uint64_t SwingLockUntil() noexcept
    {
        if (g_lockUntil && g_sink && g_sink->NowMs() >= g_lockUntil)
            return 0;                   // expired locks read as "free"
        return g_lockUntil;
    }

    void PredTick()
    {
        if (!g_sink)
            return;
        const uint64_t now = g_sink->NowMs();
        for (int s = 0; s < kMaxPeers; ++s) {
            Pred& p = g_pred[s];
            if (!p.due || now < p.due)
                continue;
            const uint8_t slot = static_cast<uint8_t>(s);
            p.due = 0;
            if (g_sink->PredBlocked(slot))
                continue;               // dodge / down / paired / busy: no speculative clip
            g_sink->PlayPuppetReaction(slot, p.sid, p.cut, 0);
            p.played = p.last = now;
            p.pain = g_pain;
            if (p.pain)
                g_sink->PainSound(slot, p.str, p.sharp);
        }
    }

    void Receive(int slot, const uint8_t* buf, size_t len)
    {
        if (!g_sink || slot < 0 || slot >= kMaxPeers || !buf || len < kMessageSize)
            return;
        const uint8_t flags = buf[0];
        uint64_t sid = 0;
        uint16_t cut = 0;
        std::memcpy(&sid, buf + 1, 8);
        std::memcpy(&cut, buf + 9, 2);
        const uint64_t now = g_sink->NowMs();
        const bool react = (flags & kFReaction) && sid != 0;

        // ext MhrRecv: bit7 with no reaction = the victim left its reaction early.
        if ((flags & kFLeft) && !react) {
            if (sid && g_puppet[slot].sid == sid) {
                g_sink->StopPuppetReaction(static_cast<uint8_t>(slot), sid, 0.45f);
                g_puppet[slot] = Puppet{};
            }
            return;
        }
        if (react && (cut < kMinCutMs || cut > kMaxCutMs))
            return;                     // not one of ours / out of range

        if (flags & (kFReaction | kFLanded))
            g_sink->HitFxConfirm(static_cast<uint8_t>(slot));

        // Attacker's provisional lock: correct it to the confirmed impact part (phased by half
        // an RTT) or release it when the victim says no reaction played.
        if (g_atk.ms && g_atk.slot == slot && now - g_atk.ms < kConfirmMs) {
            const float rtt = g_sink->RttMs(static_cast<uint8_t>(slot));
            const uint64_t age = rtt > 0.0f ? static_cast<uint64_t>(rtt * 0.5f) : 40ull;
            uint64_t until = react ? g_atk.ms + age + cut : now;
            if (until > g_atk.ms + kAtkMaxMs)
                until = g_atk.ms + kAtkMaxMs;
            g_lockUntil = until > now ? until : 0;
            g_atk.ms = 0;               // released
        }
        if ((flags & (kFReaction | kFLanded)) && (flags & kFPunch) && g_punch)
            g_sink->Punch(static_cast<uint8_t>(slot), react ? 1 : 0);

        // Predicted reaction: the victim's word confirms (cut timing) or undoes (blend out)
        // the clip we already played on its puppet.
        Pred& p = g_pred[slot];
        p.due = 0;                      // the real message wins over a pending prediction
        if (p.played && now - p.played < kConfirmMs) {
            const uint64_t played = p.played;
            p.played = 0;
            if (react) {
                if (g_puppet[slot].sid == p.sid)
                    g_puppet[slot].end = played + g_sink->ExitMsOf(sid, cut);
                if ((flags & kFPain) && !p.pain)
                    g_sink->PainSound(static_cast<uint8_t>(slot),
                                      static_cast<int>(PainStrength(flags)),
                                      PainSharp(flags));
            } else {
                if (g_puppet[slot].sid) {
                    g_sink->StopPuppetReaction(static_cast<uint8_t>(slot),
                                               g_puppet[slot].sid, 0.15f);
                    g_puppet[slot] = Puppet{};
                }
                if ((flags & kFPain) && !p.pain)
                    g_sink->PainSound(static_cast<uint8_t>(slot),
                                      static_cast<int>(PainStrength(flags)),
                                      PainSharp(flags));
            }
            return;
        }
        if (flags & kFPain)
            g_sink->PainSound(static_cast<uint8_t>(slot),
                              static_cast<int>(PainStrength(flags)), PainSharp(flags));
        if (react) {
            g_sink->PlayPuppetReaction(static_cast<uint8_t>(slot), sid, cut, 0);
            g_puppet[slot].sid = sid;
            g_puppet[slot].end = now + g_sink->ExitMsOf(sid, cut);
        }
    }

    void Tick()
    {
        VictimTick();
        PredTick();
        if (!g_sink)
            return;
        const uint64_t now = g_sink->NowMs();
        for (int s = 0; s < kMaxPeers; ++s) {
            if (g_puppet[s].end && now >= g_puppet[s].end)
                StopPuppet(static_cast<uint8_t>(s));
        }
    }

    void Reset()
    {
        g_v = Victim{};
        g_atk = Atk{};
        g_lockUntil = 0;
        for (auto& p : g_pred)
            p = Pred{};
        for (auto& p : g_puppet)
            p = Puppet{};
    }

} // namespace gameplay::melee_hit_react
