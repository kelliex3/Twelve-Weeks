#include "hawk_eyed.hpp"

#include "net-common.hpp"
#include "net-protocol.hpp"
#include "net-player.hpp"
#include "net-session.hpp"
#include "net-matchmaking.hpp"
#include "net-gui2.hpp"
#include "net-vox.hpp"
#include "kill-feed.hpp"
#include "net-debug.hpp"
#include "gameplay/loadout/purchase.hpp"

namespace gameplay {

    // MARKING
    static constexpr float kMarkFadeInSec = 0.12f;
    static constexpr float kMarkLifetime = 6.0f;
    static constexpr float kMarkFadeOutSec = 1.0f;

    static constexpr int kMarkParts = 15; // paid once per fresh mark you place

    static long s_diagMarkSet = 0; // marks we started locally
    static long s_diagMarkNet = 0; // marks received from a teammate
    static long s_diagMarkRefuse = 0; // marks refused (friendly / self / bad slot)

    static bool ApplyMark(int slot, bool fromNet)
    {
        if (slot < 0 || slot >= kMaxNetPlayers) { ++s_diagMarkRefuse; return false; }
        if (slot == net::session::MySlot()) { ++s_diagMarkRefuse; return false; }  // never yourself
        if (NetPlayers::SlotIsFriendly(slot)) { ++s_diagMarkRefuse; return false; }  // never a teammate
        NetPlayer& p = NetPlayers::BySlot(slot);
        if (!p.IsActiveRemote() || !p.spawned) { ++s_diagMarkRefuse; return false; }
        if (net::match::Get().slotState[slot].dead) { ++s_diagMarkRefuse; return false; }

        const bool fresh = !p.aimMarkActive;
        if (fresh) {
            p.aimMarkActive = true;
            p.aimMarkAge = 0.0f;
        }
        if (fromNet) ++s_diagMarkNet; else ++s_diagMarkSet;

        if (fresh && !fromNet)
            net::replication::AwardParts(kMarkParts, net::session::MySlot(), "Marked Enemy");

        if (fresh)
            TO1_LOG("[mark] %s slot%d (\"%s\") | local=%ld net=%ld refused=%ld\n",
                fromNet ? "TEAMMATE MARKED" : "I MARKED", slot, p.name,
                s_diagMarkSet, s_diagMarkNet, s_diagMarkRefuse);
        return true;
    }

    static void SendMarkToTeammates(int markedSlot)
    {
        AimMarkEvent evt{ static_cast<uint8_t>(markedSlot) };
        for (NetPlayer& t : NetPlayers::Remotes()) {
            if (!t.active) continue;
            if (!NetPlayers::SlotIsFriendly(t.netSlot)) continue; // teammates only
            net::session::SendEvent(NEVT_MARK_PLAYER, evt, static_cast<uint8_t>(t.netSlot));
        }
    }

    static int s_aimRc = -1; // pending async spherecast handle (-1 = none in flight)
    static uint64_t s_aimKickFrame = 0; // frame the cast was kicked (result ready on +1)
    static uint64_t s_aimTarget = 0; // npcSid under the reticle, if it is a markable enemy
    static bool s_aimPrevPress = false;

    void HawkEyedTick(float dt)
    {
        if (!net::gui2::InMatch() || net::gui2::ScoreboardShowing() ||
            net::gui2::PauseMenuOpen() || net::matchmaking::MyTeam() < 0) {
            s_aimRc = -1;
            s_aimTarget = 0;
            s_aimPrevPress = false;
            return;
        }

        if (dt <= 0.0f) return; // paused
        if (dt > 0.1f)  dt = 0.1f;

        if (s_aimRc == -1) {
            uint64_t start = DC::Call<"camera-through-reticle-locator">();
            if (start) {
                uint64_t fwd = DC::Call<"alloc-vector">(0, 0, 60.0f, 1.0f, 0);
                uint64_t end = DC::Call<"offset-locator">(start, fwd);
                s_aimRc = static_cast<int>(DC::Call<"kick-spherecast">(start, end, 0.25f, SID("player"), 0x107));
                s_aimKickFrame = DC::Call<"get-current-frame-count">();
            }
        }
        else {
            uint64_t frame = DC::Call<"get-current-frame-count">();
            if (frame == s_aimKickFrame + 1) {
                uint64_t hit = DC::Call<"wait-raycast-get-object">(s_aimRc);
                s_aimTarget = 0;
                if (hit) {
                    for (NetPlayer& p : NetPlayers::Remotes()) {
                        if (!p.spawned || p.npcSid != hit) continue;
                        if (!NetPlayers::SlotIsFriendly(p.netSlot)) s_aimTarget = hit;
                        break;
                    }
                }
                s_aimRc = -1;
            }
            else if (frame > s_aimKickFrame + 1) {
                s_aimRc = -1;
            }
        }

        const bool press = DC::JOYPAD_COMMAND_ACTIVE(SID("flashlight-toggle"));
        const bool justPressed = press && !s_aimPrevPress;
        s_aimPrevPress = press;

        if (s_aimTarget != 0 && justPressed && DC::Call<"player-is-aiming?">()) {
            for (NetPlayer& p : NetPlayers::Remotes()) {
                if (p.npcSid != s_aimTarget) continue;
                if (ApplyMark(p.netSlot, /*fromNet=*/false)) {
                    SendMarkToTeammates(p.netSlot);
                    net::vox::PlayForLocal(net::vox::Scenario::MarkedEnemy);
                }
                break;
            }
        }

        for (int slot = 0; slot < kMaxNetPlayers; ++slot) {
            NetPlayer& p = NetPlayers::BySlot(slot);
            if (!p.aimMarkActive) continue;

            if (!p.IsActiveRemote() || !p.spawned || !p.npcSid ||
                net::match::Get().slotState[slot].dead ||
                NetPlayers::SlotIsFriendly(slot)) {
                p.aimMarkActive = false;
                continue;
            }

            p.aimMarkAge += dt;
            const float age = p.aimMarkAge;
            float a;
            if (age >= kMarkLifetime) { p.aimMarkActive = false; continue; }
            else if (age < kMarkFadeInSec)                a = age / kMarkFadeInSec;
            else if (age < kMarkLifetime - kMarkFadeOutSec) a = 1.0f;
            else                                          a = (kMarkLifetime - age) / kMarkFadeOutSec;

            NPC::HIGHLIGHT(p.npcSid, kTeamRedR / 255.0f, kTeamRedG / 255.0f, kTeamRedB / 255.0f, a);
        }
    }

    void ApplyNetMark(int senderSlot, int markedSlot)
    {
        if (!NetPlayers::SlotIsFriendly(senderSlot)) {
            ++s_diagMarkRefuse;
            return;
        }
        ApplyMark(markedSlot, /*fromNet=*/true);
    }

    bool IsSlotMarked(int slot)
    {
        return (slot >= 0 && slot < kMaxNetPlayers) && NetPlayers::BySlot(slot).aimMarkActive;
    }

    void ClearAimMarks()
    {
        for (int i = 0; i < kMaxNetPlayers; ++i) {
            NetPlayers::BySlot(i).aimMarkActive = false;
            NetPlayers::BySlot(i).aimMarkAge = 0.0f;
        }
    }

    void HawkEyedOnSpawn(const SpawnContext& ctx)
    {
        s_aimRc = -1;
        s_aimTarget = 0;
        s_aimPrevPress = false;
        TO1_LOG("[mark] aim state cleared on %s\n", ctx.respawn ? "respawn" : "match start");
    }

    void HawkEyedOnDeath(const DeathContext& ctx)
    {
        ClearAimMarks();
        s_aimRc = -1;
        s_aimTarget = 0;
        s_aimPrevPress = false;
        TO1_LOG("[mark] marks dropped on death (killer slot%d)\n", ctx.killerSlot);
    }

    void HawkEyedReset()
    {
        ClearAimMarks();
        s_aimRc = -1;
        s_aimKickFrame = 0;
        s_aimTarget = 0;
        s_aimPrevPress = false;
        s_diagMarkSet = s_diagMarkNet = s_diagMarkRefuse = 0;
    }
}