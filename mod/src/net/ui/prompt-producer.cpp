// Native producers for tools/ext/prompt_anim.c's g_wpg / g_wpr writers.
// Only facts the mod already owns are read: the live melee-dodge / exchange
// window, the gift item in hand, the (dormant) downed core, the supply-box
// interact bridge and the one shared pad / key snapshot in prompt_anim.
// No winmm / VA detour, no second input sampler, no new wire opcode or toggle.

#include "prompt-producer.hpp"
#include "prompt-producer-core.hpp"
#include "prompt-label-core.hpp"

#include "prompt-anim.hpp"

#include "boot.hpp"
#include "engine/backpack-prop.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/local-player.hpp"
#include "engine/pad.hpp"
#include "engine/raycast.hpp"
#include "engine/window.hpp"
#include "log.hpp"                 // before engine/script.hpp (TO1_LOG template helper)
#include "engine/script.hpp"
#include "sid/sid.hpp"

#include "gameplay/melee-dodge.hpp"
#include "gameplay/melee-exchange.hpp"
#include "net/match/downed-state.hpp"
#include "net/match/match-liveness.hpp"
#include "net/match/match.hpp"
#include "net/match/melee-bridge.hpp"
#include "net/match/net-player.hpp"
#include "net/ui/net-gui2.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace net::ui::prompt_producer {

    namespace {

        using prompt_anim::core::Kind;
        namespace lc = prompt_label::core;

        uint64_t NowMs()
        {
            const double v = engine::frame::NowMs();
            return v > 0.0 ? static_cast<uint64_t>(v) : 0ull;
        }

        bool CanShow()
        {
            return !boot::ExtLoaded() && net::match::liveness::Live() &&
                   net::gui2::InMatch() && !net::gui2::PauseMenuOpen() &&
                   !net::gui2::ScoreboardShowing();
        }

        // ext's two writer slots: last writer won, so here a fixed priority
        // decides when two native features ask for the same slot in one frame.
        // A time-critical counter press beats a hold / interact label; the
        // grey slot's specific pullout / pin state beats the generic dodge wait.
        struct Candidate {
            bool      valid = false;
            int       priority = -1;
            lc::Label label;
            float     x = -1.0f, y = -1.0f;
        };

        Candidate gSlot0, gSlot1;

        void Offer(Candidate& dst, int priority, const lc::Label& label,
                   float x, float y)
        {
            if (!label.valid) return;
            if (dst.valid && priority <= dst.priority) return;
            dst.valid = true;
            dst.priority = priority;
            dst.label = label;
            dst.x = x;
            dst.y = y;
        }

        bool PeerAnchor(int peer, float& x, float& y)
        {
            x = -1.0f;
            y = -1.0f;
            if (peer < 0 || peer >= kMaxNetPlayers) return false;
            const NetPlayer& p = NetPlayers::BySlot(peer);
            if (!p.npcSid) return false;
            const Transform t = LOCATOR::GET_TRANSFORM(p.npcSid);
            return prompt_anim::Project(t.Position.x, t.Position.y + 1.0f,
                                        t.Position.z, x, y);
        }

        // ---- cached static-world LOS (the ext's LosGate) ---------------------------------
        // The kick ray is async: kick on one frame, read RayT() on later ones.
        // An unanswered / refused ray stays optimistic, like near-miss.cpp.
        std::array<uint64_t, kMaxNetPlayers> gLosJob{};
        std::array<bool,      kMaxNetPlayers> gLosBlocked{};

        void PollLos(int peer, bool want)
        {
            if (peer < 0 || peer >= kMaxNetPlayers) return;
            if (!want) {
                gLosJob[peer] = 0;
                gLosBlocked[peer] = false;
                return;
            }
            if (gLosJob[peer]) {
                const float t = engine::raycast::RayT(gLosJob[peer]);
                if (t != -1.0f) {
                    gLosBlocked[peer] = t >= 0.0f && t < 0.98f;
                    gLosJob[peer] = 0;
                }
            }
            if (gLosJob[peer]) return;
            const NetPlayer& p = NetPlayers::BySlot(peer);
            if (!p.npcSid) return;
            const Transform me = LOCATOR::GET_TRANSFORM(SID("player"));
            const Transform them = LOCATOR::GET_TRANSFORM(p.npcSid);
            gLosJob[peer] = engine::raycast::KickWorldRay(
                Vector3{ me.Position.x, me.Position.y + 1.0f, me.Position.z },
                Vector3{ them.Position.x, them.Position.y + 1.0f, them.Position.z },
                engine::raycast::kMaskStaticWorld, SID("player"));
        }

        bool LosClear(int peer) noexcept
        {
            return peer >= 0 && peer < kMaxNetPlayers && !gLosBlocked[peer];
        }

        // ---- slot 0: dodge window, then the two counter state machines ------------------
        void FeedDodgeWindow()
        {
            if (!net::match::melee_bridge::Installed()) return;
            const uint64_t now = NowMs();
            uint16_t winMs = 0;
            const int peer = gameplay::melee_dodge::Window(now, &winMs);
            if (peer < 0 || !winMs) return;
            const uint64_t end = gameplay::melee_dodge::WindowEnd();
            if (!end || now >= end) return;

            float x = 0.0f, y = 0.0f;
            PeerAnchor(peer, x, y);
            Offer(gSlot0, 1,
                  lc::Make(Kind::Wait, "", core::WaitFill(now, end, winMs)),
                  x, y);
        }

        // melee_duel_pin.c's WAIT -> PRESS -> "Too late" sequence. The native
        // duel QTE is not ported, but the live melee-exchange defender window
        // narrows the same dodge window, so this feeds the real window state.
        struct PinFeed {
            int      peer = -1;
            uint64_t open = 0, end = 0, ring = 0;
        } gPin;
        lc::CounterState gPinState;

        void FeedPin(bool oldTexts)
        {
            if (!net::match::melee_bridge::Installed()) return;
            namespace Exchange = gameplay::melee_exchange;
            const bool live = Exchange::CurrentRole() == Exchange::kDefender &&
                              Exchange::CurrentBeat() == Exchange::kAttack;
            if (!live) {
                if (!gPin.end) return;
                gPin.peer = -1;   // keep the late linger, drop the anchor
            } else {
                const uint64_t now = NowMs();
                uint16_t winMs = 0;
                const int peer = gameplay::melee_dodge::Window(now, &winMs);
                const uint64_t end = gameplay::melee_dodge::WindowEnd();
                // The dodge core may report a different peer's swing as the
                // best window; only label the exchange defender's own slot.
                if (peer >= 0 && Exchange::WindowEnd(
                        static_cast<uint8_t>(peer), nullptr) == 0)
                    return;
                if (peer >= 0 && end) {
                    const uint64_t open = end > winMs ? end - winMs : 1ull;
                    if (end != gPin.end) gPinState.Reset();
                    gPin.peer = peer;
                    gPin.open = open;
                    gPin.end = end;
                    gPin.ring = lc::RingAt(open, end, lc::kPinRingMs,
                                           lc::kPinGraceMs);
                }
                // A closed window still has to arm / show its 150 ms linger.
            }

            const uint64_t now = NowMs();
            const lc::Label l = lc::Counter(now, true, gPin.open, gPin.end,
                                            gPin.ring, prompt_anim::PressEdge(),
                                            oldTexts, false, lc::Style::Pin,
                                            gPinState);
            float x = 0.0f, y = 0.0f;
            PeerAnchor(gPin.peer, x, y);
            Offer(gSlot0, 2, l, x, y);
            if (l.valid && (l.kind == Kind::Press || l.kind == Kind::Text)) {
                Offer(gSlot1, 5, l, x, y);
                // The ext's regular record normally goes through the engine's
                // interact widget. With no widget writer here, slot 0 carries
                // the label unless the existing selfdraw opt-in owns slot 1.
                if (!prompt_anim::RegularOptIn())
                    Offer(gSlot0, 4, l, x, y);
            }
            if (!gPinState.lateUntil && !l.valid) gPin.end = 0;
        }

        // melee_pullout.c's wait / escape / early / late sequence, fed each frame
        // by the native family-5 pull-out bridge via NotePullout().
        struct PulloutFeed {
            int      peer = -1;
            bool     on = false;
            uint64_t open = 0, end = 0;
        } gPull;
        lc::CounterState gPullState;

        void FeedPullout(bool oldTexts)
        {
            if (!gPull.end) return;
            const uint64_t now = NowMs();
            const uint64_t open = gPull.open ? gPull.open
                : (gPull.end > lc::kPulloutLeadMs ? gPull.end - lc::kPulloutLeadMs : 1ull);
            const lc::Label l = lc::Counter(now, gPull.on, open, gPull.end,
                                            open, prompt_anim::PressEdge(),
                                            oldTexts, false, lc::Style::Pullout,
                                            gPullState);
            float x = 0.0f, y = 0.0f;
            PeerAnchor(gPull.peer, x, y);
            Offer(gSlot0, 3, l, x, y);
            if (l.valid && (l.kind == Kind::Press || l.kind == Kind::Text)) {
                Offer(gSlot1, 6, l, x, y);
                if (!prompt_anim::RegularOptIn())
                    Offer(gSlot0, 5, l, x, y);
            }
            if (!gPullState.lateUntil && !l.valid) gPull.end = 0;
        }

        // ---- slot 1: revive, gift, the counter press, supply box ------------------------
        void FeedSupplyBox()
        {
            if (engine::local_player::IsAiming() ||
                NetPlayers::supplyBoxes.empty())
                return;

            const float nowSec = net::match::Get().NowSec();
            NetPlayers::SupplyBox* best = nullptr;
            float bestDist = core::kInteractReach + 1.0f;
            for (NetPlayers::SupplyBox& box : NetPlayers::supplyBoxes) {
                if (!box.CanOpenSupplyBox(nowSec)) continue;
                const float d = box.GetDistanceToPlayer();
                if (d < bestDist) {
                    bestDist = d;
                    best = &box;
                }
            }
            if (!best || !core::SupplyPrompt(bestDist, false, true)) return;

            const Transform t = LOCATOR::GET_TRANSFORM(best->boxSID);
            float x = 0.0f, y = 0.0f;
            if (!prompt_anim::Project(t.Position.x, t.Position.y + 0.6f,
                                      t.Position.z, x, y))
                return;
            Offer(gSlot1, 0, lc::Make(Kind::Press, "Open"), x, y);
        }

        // gift.c: the box is a real engine item; the peer's stock mask arrives
        // on the ext's MSG_SK_GIFTSTOCK, which has no native bridge yet.
        int gGiftKind = -1;
        std::array<uint8_t,  kMaxNetPlayers> gGiftRoom{};
        std::array<uint64_t, kMaxNetPlayers> gGiftRoomAt{};

        void FeedGift(uint64_t now)
        {
            if (gGiftKind < 0 || gGiftKind > 3) return;
            if (engine::backpack_prop::CurrentWeapon() != SID("gift")) return;

            const Transform me = LOCATOR::GET_TRANSFORM(SID("player"));
            NetPlayer* best = nullptr;
            float bestXZ = lc::kHealHoldXZ + 1.0f;
            for (NetPlayer& p : NetPlayers::Remotes()) {
                if (!NetPlayers::SlotIsFriendly(p.netSlot) || !p.npcSid) continue;
                const Transform them = LOCATOR::GET_TRANSFORM(p.npcSid);
                const float dx = them.Position.x - me.Position.x;
                const float dy = them.Position.y - me.Position.y;
                const float dz = them.Position.z - me.Position.z;
                if (!lc::HealReach(dx, dy, dz)) continue;
                const float xz = dx * dx + dz * dz;
                if (xz >= bestXZ) continue;
                bestXZ = xz;
                best = &p;
            }
            if (!best) return;
            PollLos(best->netSlot, true);
            if (!LosClear(best->netSlot)) return;

            const bool known = gGiftRoomAt[best->netSlot] &&
                               now >= gGiftRoomAt[best->netSlot] &&
                               now - gGiftRoomAt[best->netSlot] <= 8000;
            const bool hasRoom = known &&
                ((gGiftRoom[best->netSlot] >> gGiftKind) & 1u) != 0;
            const lc::Label l = lc::Gift(true, true, known, hasRoom);
            float x = 0.0f, y = 0.0f;
            PeerAnchor(best->netSlot, x, y);
            Offer(gSlot1, 3, l, x, y);
            if (!prompt_anim::RegularOptIn()) Offer(gSlot0, 1, l, x, y);
        }

        // t2r-ext.c's Revive writer. The downed bridge is deliberately inert
        // (kEnabled == false), so PeerState() stays "up" until that bridge is
        // enabled; the label path itself is native and uses the ported RvProg.
        void FeedRevive()
        {
            namespace Downed = net::match::downed;
            const bool held = engine::pad::Held("interact") ||
                              engine::window::KeyDown('E');
            const Transform me = LOCATOR::GET_TRANSFORM(SID("player"));
            for (NetPlayer& p : NetPlayers::Remotes()) {
                if (!NetPlayers::SlotIsFriendly(p.netSlot) || !p.npcSid) continue;
                if (Downed::PeerState(static_cast<uint8_t>(p.netSlot)) !=
                    Downed::kStateDown)
                    continue;
                const float progress = Downed::ReviveProgress(
                    static_cast<uint8_t>(p.netSlot));
                const Transform them = LOCATOR::GET_TRANSFORM(p.npcSid);
                const float dx = them.Position.x - me.Position.x;
                const float dy = them.Position.y - me.Position.y;
                const float dz = them.Position.z - me.Position.z;
                if (!lc::ReviveReach(dx, dy, dz, progress > 0.0f)) continue;
                PollLos(p.netSlot, true);
                if (!LosClear(p.netSlot)) continue;
                float x = 0.0f, y = 0.0f;
                PeerAnchor(p.netSlot, x, y);
                const lc::Label l = lc::Revive(true, held, progress);
                Offer(gSlot1, 4, l, x, y);
                if (!prompt_anim::RegularOptIn()) Offer(gSlot0, 1, l, x, y);
            }
        }

    } // namespace

    void NotePullout(int peer, bool on, uint64_t openMs, uint64_t endMs)
    {
        if (on && (gPull.peer != peer || gPull.end != endMs))
            gPullState.Reset();
        gPull.peer = peer;
        gPull.on = on;
        gPull.open = openMs;
        gPull.end = endMs;
    }

    void NoteGiftCarried(int kind)
    {
        gGiftKind = kind;
    }

    void SetGiftPeerRoom(int peer, bool known, uint8_t roomMask)
    {
        if (peer < 0 || peer >= kMaxNetPlayers) return;
        gGiftRoom[peer] = known ? roomMask : 0;
        gGiftRoomAt[peer] = known ? NowMs() : 0;
    }

    void Tick(uint64_t hudNow)
    {
        if (!CanShow()) return;
        gSlot0 = {};
        gSlot1 = {};
        const bool oldTexts = engine::flag::Present("t2r-prompt-anim-old.txt");

        FeedDodgeWindow();
        FeedPin(oldTexts);
        FeedPullout(oldTexts);
        FeedSupplyBox();
        FeedGift(hudNow);
        FeedRevive();

        if (gSlot0.valid)
            prompt_anim::Show(0, gSlot0.label.kind, gSlot0.label.text,
                              gSlot0.label.progress, gSlot0.x, gSlot0.y);
        if (gSlot1.valid)
            prompt_anim::Show(1, gSlot1.label.kind, gSlot1.label.text,
                              gSlot1.label.progress, gSlot1.x, gSlot1.y);
    }

    void Reset()
    {
        gSlot0 = {};
        gSlot1 = {};
        gPin = {};
        gPinState.Reset();
        gPull = {};
        gPullState.Reset();
        gGiftKind = -1;
        gGiftRoom.fill(0);
        gGiftRoomAt.fill(0);
        gLosJob.fill(0);
        gLosBlocked.fill(false);
    }

} // namespace net::ui::prompt_producer
