#include "downed-state.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace net::match::downed {
namespace {

    struct Peer {
        uint8_t  state = kStateUp;
        uint8_t  knockDir = 0;        // raw byte 2 (0 = default fall)
        uint8_t  deathDir = 0;        // raw byte 3 while dying
        bool     supine = false;      // byte 3 == 1 while down
        float    bleedSecs = 0.0f;    // byte 4 - 0.5 (0 = old peer)
        uint8_t  epoch = 0;
        uint64_t epochMs = 0;
        uint64_t downFrame = 0;
        uint64_t revivedFrame = 0;    // we completed a revive on them (stale resend guard)
    };

    struct Hold {
        uint64_t startMs = 0;         // 0 = not attached
        float    secs = 0.0f;         // that reviver's revive time (skills change it)
    };

    Sink*  g_sink = nullptr;
    Config g_config{};

    Peer g_peers[kMaxPeers]{};
    Hold g_holds[kMaxPeers][kMaxPeers]{};

    bool     g_down = false;
    bool     g_localDying = false;      // ext g_downDying (lethal hit / bleed-out while down)
    uint64_t g_dyingUntilFrame = 0;     // ext g_uc4DyingUntil (UC4 death-clip window)
    uint8_t  g_state = kStateUp;
    uint8_t  g_downer = 0;            // slot that downed us (0xFE on a revive)
    uint8_t  g_knockDir = 0;          // our fall direction (0 = default)
    bool     g_supine = false;        // we went down lying on our back
    uint8_t  g_epoch = 0;             // our send epoch, ++ on every state change
    uint64_t g_downMs = 0;
    uint64_t g_downFrame = 0;
    float    g_bleedSecs = kBleedSecs;
    float    g_baseBleedSecs = kBleedSecs;
    uint64_t g_bleedPauseMs = 0;
    uint64_t g_lastDownMs = 0;
    float    g_lastHp = -1.0f;
    uint8_t  g_downCount = 0;         // downs this life (legacy bleed shortening)
    uint8_t  g_reviveCount = 0;       // revives this life (2013 bleed shortening)
    uint64_t g_lastResendFrame = 0;

    uint8_t g_upBody[6]{};
    int     g_upLeft = 0;
    uint64_t g_upNextMs = 0;

    uint8_t  g_localReviver = kMaxPeers;
    float    g_localReviverSecs = 0.0f;
    uint64_t g_localReviverMs = 0;
    uint64_t g_lastTickMs = 0;

    uint64_t Now()
    {
        return g_sink ? g_sink->NowMs() : 0;
    }

    uint64_t Frame()
    {
        return g_sink ? g_sink->Frame() : 0;
    }

    uint8_t Local()
    {
        return g_sink ? g_sink->LocalSlot() : 0;
    }

    bool InMatch()
    {
        return g_sink && g_sink->InMatch();
    }

    float DownPool()
    {
        float mul = g_sink ? g_sink->DownHpMul() : 1.0f;
        if (!(mul > 0.0f))
            mul = 1.0f;
        return kDownHp * mul;
    }

    // The ext's base bleed-out for this down. 2013: mode base x (1 - min(revives x .25, .5)).
    // Legacy: mode base x {1, .66, .4} by the down count this life.
    float ComputeBleed()
    {
        float base = g_config.bleedSecs;
        if (g_config.oldRules) {
            static constexpr float kBleedByDown[3] = { 1.0f, 0.66f, 0.4f };
            const int index = std::clamp<int>(g_downCount, 1, 3) - 1;
            base *= kBleedByDown[index];
        } else {
            float cut = std::min<float>(g_reviveCount * 0.25f, 0.5f);
            base *= 1.0f - cut;
        }
        float mul = g_sink ? g_sink->BleedMul() : 1.0f;
        if (mul > 0.0f)
            base *= mul;
        return base > 0.0f ? base : 0.1f;
    }

    void SendBody(const uint8_t body[6])
    {
        if (g_sink)
            g_sink->SendDowned(body);
    }

    void ClearLocalReviverHold()
    {
        if (g_localReviver < kMaxPeers) {
            const uint8_t target = Local();
            g_holds[target][g_localReviver].startMs = 0;
            g_holds[target][g_localReviver].secs = 0.0f;
        }
        g_localReviver = kMaxPeers;
        g_localReviverSecs = 0.0f;
        g_localReviverMs = 0;
    }

} // namespace

    void SetSink(Sink* sink)
    {
        g_sink = sink;
    }

    void SetConfig(const Config& config)
    {
        g_config = config;
    }

    Config GetConfig()
    {
        return g_config;
    }

    bool IsDown() noexcept
    {
        return g_down;
    }

    bool LocalDying() noexcept
    {
        return g_localDying || (g_dyingUntilFrame && Frame() < g_dyingUntilFrame);
    }

    void SetLocalDying(bool on)
    {
        g_localDying = on;
    }

    void SetDyingUntilFrame(uint64_t frame)
    {
        g_dyingUntilFrame = frame;
    }

    uint8_t LocalState() noexcept
    {
        // ext bloodbuild.c: g_downed ? (g_downDying || g_uc4DyingUntil ? 2 : 1) : 0
        return !g_down ? kStateUp : (LocalDying() ? kStateDying : kStateDown);
    }

    uint8_t DownerSlot() noexcept
    {
        return g_downer;
    }

    uint8_t Epoch() noexcept
    {
        return g_epoch;
    }

    uint64_t DownMs() noexcept
    {
        return g_downMs;
    }

    float BleedSecs() noexcept
    {
        return g_bleedSecs;
    }

    float BleedFraction()
    {
        if (!g_down || !g_downMs || !(g_bleedSecs > 0.0f))
            return 0.0f;
        uint64_t elapsed = Now() - g_downMs;
        if (elapsed > g_bleedPauseMs)
            elapsed -= g_bleedPauseMs;
        else
            elapsed = 0;
        return static_cast<float>(static_cast<double>(elapsed) /
                                  (static_cast<double>(g_bleedSecs) * 1000.0));
    }

    bool BeginDown(uint8_t downer, uint8_t knockDir, bool supine)
    {
        if (g_down) {
            if (g_sink)
                g_sink->Log("downed: already down - second down ignored "
                            "(no second award / bleed-out restart)");
            return false;
        }
        const uint64_t now = Now();
        g_down = true;
        g_state = kStateDown;
        g_localDying = false;             // a fresh down is "crawling", never "dying"
        g_dyingUntilFrame = 0;
        g_downer = downer;
        g_knockDir = knockDir;
        g_supine = supine;
        g_downMs = now ? now : 1;
        g_downFrame = Frame();
        g_bleedPauseMs = 0;
        g_lastDownMs = 0;
        g_lastHp = -1.0f;
        g_lastResendFrame = Frame() ? Frame() : 1; // no duplicate resend on the first frame
        ++g_epoch;
        ++g_downCount;
        g_bleedSecs = ComputeBleed();
        g_baseBleedSecs = g_bleedSecs;

        // A fresh down drops any revive that was running on us.
        ClearLocalReviverHold();

        uint8_t body[6];
        BuildDowned(body, kStateDown, downer, g_knockDir, g_supine ? 1 : 0,
                    g_bleedSecs);
        SendBody(body);
        if (g_sink)
            g_sink->OnDownBegin(downer, g_bleedSecs);
        return true;
    }

    void EndDown(End why, uint8_t source)
    {
        if (!g_down)
            return;
        const uint64_t now = Now();
        const bool revived = why == End::kRevived;
        if (revived)
            ++g_reviveCount;
        if (revived) {                    // a revive ends the dying window too; a death keeps it
            g_localDying = false;
            g_dyingUntilFrame = 0;
        }
        g_down = false;
        g_state = kStateUp;
        g_downer = revived ? kRevivedDowner : source;
        ++g_epoch;
        g_downMs = 0;
        g_bleedPauseMs = 0;
        g_lastHp = -1.0f;
        g_lastDownMs = 0;
        ClearLocalReviverHold();

        uint8_t body[6];
        BuildDowned(body, kStateUp, g_downer, 0, 0, g_bleedSecs);
        SendBody(body);

        // The up / dormant message is single-shot UDP; repeat it three times so a lost
        // copy cannot leave a peer showing a standing player as downed forever.
        std::memcpy(g_upBody, body, sizeof g_upBody);
        g_upLeft = kUpResendCount;
        g_upNextMs = now + kUpResendMs;

        if (g_sink)
            g_sink->OnDownEnd(why, source);
    }

    bool HitWhileDown(float dmg, float hpBefore)
    {
        if (!g_down || !(dmg > 0.0f))
            return false;
        const uint64_t now = Now();
        // 1e6 = execution kill marker: never covered by down protection.
        if (dmg < 100000.0f && g_downMs && now - g_downMs < kProtectMs) {
            if (g_sink)
                g_sink->OnProtectedHit(dmg);
            return false;
        }
        const float pool = DownPool();
        const float hpAfter = hpBefore - dmg;
        // A hit on the pool shortens this down's bleed-out by the same share of the
        // time. The ext only starts tracking after 30 frames (the pool is set after
        // the down) and never lets the cut land sooner than 1 s after the hit.
        if (g_lastHp > 0.0f && pool > 1.0f && g_baseBleedSecs > 0.5f &&
            Frame() - g_downFrame >= 30) {
            const float cut = dmg / pool * g_baseBleedSecs;
            const float before = g_bleedSecs;
            g_bleedSecs = std::max<float>(g_bleedSecs - cut, 0.1f);
            uint64_t elapsed = now - g_downMs;
            if (elapsed > g_bleedPauseMs)
                elapsed -= g_bleedPauseMs;
            else
                elapsed = 0;
            g_bleedSecs = std::max<float>(
                g_bleedSecs,
                static_cast<float>(static_cast<double>(elapsed) / 1000.0) + 1.0f);
            if (g_sink) {
                char text[160];
                std::snprintf(text, sizeof text,
                              "downed: hit for %.0f while downed - bleed-out %.1f s -> %.1f s",
                              dmg, before, g_bleedSecs);
                g_sink->Log(text);
            }
        }
        if (hpAfter <= 0.0f)
            return true;
        if (g_sink)
            g_sink->OnDownHit(dmg, hpAfter);
        return false;
    }

    void SetHold(uint8_t target, uint8_t reviver, bool on, float secs, uint64_t startMs)
    {
        if (target >= kMaxPeers || reviver >= kMaxPeers)
            return;
        Hold& hold = g_holds[target][reviver];
        hold.startMs = on ? (startMs ? startMs : (Now() ? Now() : 1)) : 0;
        hold.secs = secs;
    }

    void ClearHolds(uint8_t target)
    {
        if (target < kMaxPeers)
            std::memset(g_holds[target], 0, sizeof g_holds[target]);
    }

    int ReviveRank(uint8_t target, uint8_t reviver)
    {
        if (target >= kMaxPeers || reviver >= kMaxPeers ||
            !g_holds[target][reviver].startMs)
            return -1;
        const Hold& self = g_holds[target][reviver];
        int rank = 0;
        for (uint8_t r = 0; r < kMaxPeers; ++r) {
            if (r == reviver)
                continue;
            const Hold& other = g_holds[target][r];
            if (other.startMs &&
                (other.startMs < self.startMs ||
                 (other.startMs == self.startMs && r < reviver)))
                ++rank;
        }
        return rank;
    }

    float ReviveProgress(uint8_t target)
    {
        if (target >= kMaxPeers)
            return 0.0f;
        const uint64_t now = Now();
        float progress = 0.0f;
        for (uint8_t r = 0; r < kMaxPeers; ++r) {
            const Hold& hold = g_holds[target][r];
            if (!hold.startMs || now < hold.startMs)
                continue;
            const int rank = ReviveRank(target, r);
            const float rate = rank <= 0 ? 1.0f
                             : rank == 1 ? g_config.coRate2
                                         : g_config.coRate3;
            const float secs = hold.secs > 0.2f ? hold.secs : 5.0f;
            progress += static_cast<float>(now - hold.startMs) / 1000.0f *
                        rate / secs;
        }
        return progress;
    }

    void SetLocalReviver(uint8_t slot, float secs)
    {
        if (slot >= kMaxPeers)
            return;
        if (g_localReviver != slot)
            ClearLocalReviverHold();
        g_localReviver = slot;
        g_localReviverSecs = secs;
        g_localReviverMs = Now();
        SetHold(Local(), slot, true, secs, g_localReviverMs);
    }

    void ClearLocalReviver()
    {
        ClearLocalReviverHold();
    }

    bool BeingRevived() noexcept
    {
        return g_localReviver < kMaxPeers;
    }

    void ReceiveRevive(uint8_t from)
    {
        ClearHolds(Local());
        if (!g_down)
            return;
        if (g_sink)
            g_sink->Log("downed: revive received");
        EndDown(End::kRevived, from);
        if (g_sink)
            g_sink->OnRevived(from);
    }

    void CompleteRevive(uint8_t target, uint8_t reviver)
    {
        if (target >= kMaxPeers)
            return;
        g_peers[target].revivedFrame = Frame();
        g_peers[target].state = kStateUp;
        ClearHolds(target);
        if (g_sink)
            g_sink->SendRevive(reviver);
        if (target == Local() && g_down)
            ReceiveRevive(reviver);
    }

    uint8_t BuildDowned(uint8_t out[6], uint8_t state, uint8_t downer,
                        uint8_t knockDir, uint8_t mix, float bleedSecs)
    {
        const float bs = bleedSecs + 0.5f;
        out[0] = state;
        out[1] = downer;
        out[2] = static_cast<uint8_t>(knockDir + 1); // 0 = no direction
        out[3] = mix;
        out[4] = static_cast<uint8_t>(bs > 255.0f ? 255 :
                                      bs < 0.0f ? 0 : bs);
        out[5] = g_epoch;
        return 6;
    }

    bool ReceiveDowned(uint8_t from, const uint8_t* buf, size_t len)
    {
        if (from >= kMaxPeers || !buf || len < 1)
            return false;
        const uint8_t state = buf[0];
        if (state > kStateDying)
            return false;
        Peer& peer = g_peers[from];
        const uint64_t now = Now();

        // Epoch guard: a message older than the newest one seen in the last 10 s is
        // a reordered / late copy. Same epoch = the 1 s resend of the current state.
        if (len >= 6) {
            const uint8_t epoch = buf[5];
            if (peer.epochMs && now - peer.epochMs < kEpochWindowMs &&
                static_cast<int8_t>(epoch - peer.epoch) < 0) {
                if (g_sink)
                    g_sink->Log("downed: reordered state ignored (older epoch)");
                return false;
            }
            peer.epoch = epoch;
            peer.epochMs = now;
        }
        if (state == kStateDown && len >= 3)
            peer.knockDir = buf[2];
        if (state == kStateDown && len >= 4)
            peer.supine = buf[3] == 1;
        if (state == kStateDying && len >= 4)
            peer.deathDir = buf[3];
        if (state && len >= 5)
            peer.bleedSecs = buf[4] > 0 ? static_cast<float>(buf[4]) - 0.5f : 0.0f;

        if (state == peer.state)
            return false;
        // A "downed" resend sent before our MSG_REVIVE arrived is stale.
        if (state == kStateDown && peer.state == kStateUp && peer.revivedFrame &&
            Frame() - peer.revivedFrame < kStaleReviveFrames)
            return false;

        const uint8_t downer = len >= 2 ? buf[1] : 0;
        peer.state = state;
        if (state == kStateDown) {
            peer.downFrame = Frame() ? Frame() : 1;
            peer.revivedFrame = 0;
        }
        if (g_sink)
            g_sink->OnPeerState(from, state, downer);
        return true;
    }

    uint8_t PeerState(uint8_t slot) noexcept
    {
        return slot < kMaxPeers ? g_peers[slot].state : kStateUp;
    }

    uint8_t PeerKnockDir(uint8_t slot) noexcept
    {
        return slot < kMaxPeers ? g_peers[slot].knockDir : 0;
    }

    uint8_t PeerDeathDir(uint8_t slot) noexcept
    {
        return slot < kMaxPeers ? g_peers[slot].deathDir : 0;
    }

    bool PeerSupine(uint8_t slot) noexcept
    {
        return slot < kMaxPeers && g_peers[slot].supine;
    }

    float PeerBleed(uint8_t slot) noexcept
    {
        return slot < kMaxPeers ? g_peers[slot].bleedSecs : 0.0f;
    }

    uint64_t PeerDownFrame(uint8_t slot) noexcept
    {
        return slot < kMaxPeers ? g_peers[slot].downFrame : 0;
    }

    void Tick()
    {
        const uint64_t now = Now();
        const uint64_t frame = Frame();
        const uint64_t dt = g_lastTickMs && now > g_lastTickMs
                          ? std::min<uint64_t>(now - g_lastTickMs, 250)
                          : 0;
        g_lastTickMs = now;
        if (!InMatch())
            return;

        // Expire holds whose "stopped" was lost (ext RvRefresh's 2 x (secs + 4) test).
        for (uint8_t target = 0; target < kMaxPeers; ++target) {
            for (uint8_t reviver = 0; reviver < kMaxPeers; ++reviver) {
                Hold& hold = g_holds[target][reviver];
                if (hold.startMs &&
                    now - hold.startMs >
                        static_cast<uint64_t>((hold.secs + 4.0f) * 2000.0f))
                    hold.startMs = 0;
            }
        }
        if (BeingRevived() && g_localReviver < kMaxPeers &&
            !g_holds[Local()][g_localReviver].startMs)
            ClearLocalReviverHold();

        if (!g_down) {
            if (g_upLeft > 0 && now >= g_upNextMs) {
                SendBody(g_upBody);
                --g_upLeft;
                g_upNextMs = now + kUpResendMs;
            }
            return;
        }

        // The state-1 resend covers lost packets (ext: every 60 frames while down).
        if (g_lastResendFrame == 0 || frame - g_lastResendFrame >= kDownResendFrames) {
            g_lastResendFrame = frame ? frame : 1;
            uint8_t body[6];
            BuildDowned(body, kStateDown, g_downer, g_knockDir,
                        g_supine ? 1 : 0, g_bleedSecs);
            SendBody(body);
        }

        // The local reviver timeout and the pause both use the real-time clock.
        if (BeingRevived()) {
            if (now - g_localReviverMs >
                static_cast<uint64_t>((g_localReviverSecs + 3.0f) * 1000.0f)) {
                if (g_sink)
                    g_sink->Log("downed: revive hold timed out");
                ClearLocalReviverHold();
            } else {
                g_bleedPauseMs += dt;
            }
        }

        // Start tracking the pool 30 frames in (the ext ignores the 100 -> 50 set-up).
        if (g_lastDownMs != g_downMs) {
            if (frame - g_downFrame < 30)
                return;
            g_lastDownMs = g_downMs;
            g_lastHp = g_sink ? g_sink->LocalHp() : -1.0f;
            g_baseBleedSecs = g_bleedSecs;
        } else if (g_sink) {
            g_lastHp = g_sink->LocalHp();
        }

        // 2013 skips the death check while a reviver is attached; legacy keeps the
        // timer running and checks anyway.
        uint64_t elapsed = now - g_downMs;
        if (elapsed > g_bleedPauseMs)
            elapsed -= g_bleedPauseMs;
        else
            elapsed = 0;
        if (elapsed >= static_cast<uint64_t>(g_bleedSecs * 1000.0) &&
            (g_config.oldRules || !BeingRevived())) {
            if (g_sink)
                g_sink->Log("downed: bled out");
            const uint8_t downer = g_downer;
            if (g_sink)
                g_sink->OnBleedOut(downer);
            EndDown(End::kDied, downer);
        }
    }

    void Reset()
    {
        g_down = false;
        g_localDying = false;
        g_dyingUntilFrame = 0;
        g_state = kStateUp;
        g_downer = 0;
        g_epoch = 0;
        g_downMs = 0;
        g_downFrame = 0;
        g_bleedSecs = g_config.bleedSecs;
        g_baseBleedSecs = g_bleedSecs;
        g_bleedPauseMs = 0;
        g_lastDownMs = 0;
        g_lastHp = -1.0f;
        g_downCount = 0;
        g_reviveCount = 0;
        g_lastResendFrame = 0;
        g_upLeft = 0;
        g_upNextMs = 0;
        g_lastTickMs = 0;
        g_localReviver = kMaxPeers;
        g_localReviverSecs = 0.0f;
        g_localReviverMs = 0;
        std::memset(g_peers, 0, sizeof g_peers);
        std::memset(g_holds, 0, sizeof g_holds);
    }

    void ResetPerLife()
    {
        g_localDying = false;
        g_dyingUntilFrame = 0;
        g_downCount = 0;
        g_reviveCount = 0;
    }

    void OnPeerLeft(uint8_t slot)
    {
        if (slot >= kMaxPeers)
            return;
        g_peers[slot] = Peer{};
        for (uint8_t target = 0; target < kMaxPeers; ++target)
            g_holds[target][slot] = Hold{};
        std::memset(g_holds[slot], 0, sizeof g_holds[slot]);
        if (g_localReviver == slot)
            ClearLocalReviverHold();
    }

} // namespace net::match::downed
