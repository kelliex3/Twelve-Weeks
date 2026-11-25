// Native gift bridge: exact ext stock / give / ACK wire, real inventory calls,
// shared prompt sampling. Every effect yields while t2r-ext.dll is loaded.

#include "gift.hpp"
#include "gift-core.hpp"

#include "boot.hpp"
#include "engine/backpack-prop.hpp"
#include "engine/exe_side.hpp"
#include "engine/frame.hpp"
#include "engine/local-player.hpp"
#include "engine/raycast.hpp"
#include "engine/script.hpp"
#include "log.hpp"
#include "match.hpp"
#include "net/match/downed-state.hpp"
#include "net/match/match-liveness.hpp"
#include "net/match/net-player.hpp"
#include "net/online/net-session.hpp"
#include "net/protocol/net-protocol.hpp"
#include "net/ui/notify.hpp"
#include "net/ui/net-gui2.hpp"
#include "net/ui/prompt-anim.hpp"
#include "net/ui/prompt-producer.hpp"
#include "sid/sid.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace net::gift {

    namespace {
        namespace core = net::gift::core;

        bool g_installed = false;

        uint64_t ClockMs()
        {
            const double ms = engine::frame::NowMs();
            return ms > 0.0 ? static_cast<uint64_t>(ms) : 0ull;
        }

        bool NativeOwner()
        {
            return kEnabled && !boot::ExtLoaded() && net::match::liveness::Live();
        }

        const char* KindName(int kind) noexcept
        {
            static const char* const names[core::kKindCount] = {
                "Health Kit", "Molotov Cocktail", "Smoke Bomb", "Nail Bomb"
            };
            return core::ValidKind(kind) ? names[kind] : "gift";
        }

        void Toast(const char* text, float secs)
        {
            if (text)
                net::notify::Push(net::notify::NTF_CRAFT, text, secs);
        }

        uint64_t Qty(uint64_t sid)
        {
            const uint64_t q = DC::Call_V2<"player-inventory-get-item-quantity">(sid);
            return q == ~0ULL ? 0ull : q;
        }

        void Add(uint64_t sid, uint64_t n)
        {
            DC::Call_V2<"player-inventory-add-item-quantity">(sid, n, 0);
        }

        void Consume(uint64_t sid, uint64_t n)
        {
            DC::Call_V2<"player-inventory-consume-item">(sid, n, 0);
        }

        void RemoveGiftItem()
        {
            // The gift is not an ammo pool: set-total does not take it mid-match,
            // so consume the real item. The bounded loop also stops a native
            // that answers the consume call without changing the quantity.
            for (int i = 0; i < 16 && Qty(core::kGiftSid); ++i)
                Consume(core::kGiftSid, 1);
        }

        void LocalRoom(uint64_t qty[core::kKindCount])
        {
            for (int k = 0; k < core::kKindCount; ++k)
                qty[k] = Qty(core::kItemSid[k]);
        }

        void SendRaw(uint8_t type, const void* body, uint8_t size)
        {
            if (!NativeOwner())
                return;
            net::session::SendEventRaw(type, body, size);
        }

        void SendTo(uint8_t slot, uint8_t type, const void* body, uint8_t size)
        {
            if (!NativeOwner() || slot >= kMaxNetPlayers)
                return;
            net::session::SendEventRaw(type, body, size, slot);
        }

        int  gKind = -1;
        std::array<uint8_t, 4> gQueue{};
        int  gQueueN = 0;
        uint8_t  gLastMask = 0xFF;
        uint64_t gLastStock = 0;
        uint64_t gRegrantAt = 0;

        // Native craft producer: only craft-menu quantity edges are counted.
        core::CraftCounter gCraft;
        bool               gHadGift = false;

        uint8_t  gRoom[kMaxNetPlayers]{};
        uint64_t gRoomAt[kMaxNetPlayers]{};
        uint64_t gInAt[kMaxNetPlayers]{};

        int      gPending = -1;
        int      gPendingKind = -1;
        uint64_t gPendingUntil = 0;
        std::atomic<int> gAck{0};

        std::array<uint64_t, kMaxNetPlayers> gLosJob{};
        std::array<bool,      kMaxNetPlayers> gLosBlocked{};

        void PollLos(int slot, bool want)
        {
            if (slot < 0 || slot >= kMaxNetPlayers)
                return;
            if (!want) {
                gLosJob[slot] = 0;
                gLosBlocked[slot] = false;
                return;
            }
            if (gLosJob[slot]) {
                const float t = engine::raycast::RayT(gLosJob[slot]);
                if (t != -1.0f) {
                    gLosBlocked[slot] = t >= 0.0f && t < 0.98f;
                    gLosJob[slot] = 0;
                }
            }
            if (gLosJob[slot])
                return;
            const NetPlayer& p = NetPlayers::BySlot(slot);
            if (!p.npcSid)
                return;
            const Transform me = LOCATOR::GET_TRANSFORM(SID("player"));
            const Transform them = LOCATOR::GET_TRANSFORM(p.npcSid);
            gLosJob[slot] = engine::raycast::KickWorldRay(
                Vector3{ me.Position.x, me.Position.y + 1.0f, me.Position.z },
                Vector3{ them.Position.x, them.Position.y + 1.0f, them.Position.z },
                engine::raycast::kMaskStaticWorld, SID("player"));
        }

        bool LosClear(int slot) noexcept
        {
            return slot >= 0 && slot < kMaxNetPlayers && !gLosBlocked[slot];
        }

        bool Alive()
        {
            return engine::local_player::Health() > 0.0f &&
                   !net::match::downed::IsDown() &&
                   !PLAYER::IS_IN_STATE(SID("Death"));
        }

        // The engine grants no gift box natively, so only the bridge's own
        // Add(core::kGiftSid, 1) can create one. Naming an engine-granted box
        // is still a real seam: a rising 0 -> 1 edge with no named kind gets a
        // random contents kind instead of staying hidden forever.
        int RandomKind() noexcept
        {
            return static_cast<int>(std::rand() % core::kKindCount);
        }

        // Crafter II / III level of the player's saved GEAR UP loadout. The
        // launcher writes t2r-loadout-launcher.json next to the exe whenever
        // GEAR UP is saved, with the ACTIVE custom loadout's skills as
        //   "skills":[{"id":"SK_CRAFTER","lv":3}, ...].
        // That is the only Crafter level source available when t2r-ext.dll is
        // absent (the ext owns the lobby skill slots and t2r-loadout.json).
        // Read lazily on a 2 s TTL; 0 = unknown / no usable level, which maps
        // to the conservative Crafter II (every 3rd) cadence.
        int LocalCraftLevel()
        {
            static int    level = 0;
            static uint64_t at = 0;
            const uint64_t now = ::GetTickCount64();
            if (at && now - at < 2000)
                return level;
            at = now ? now : 1;

            FILE* f = engine::exe_side::Open("t2r-loadout-launcher.json", "rb");
            if (!f)
                f = engine::exe_side::Open("t2r-loadout.json", "rb");
            if (!f)
                return level;

            char buf[32768];
            const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
            std::fclose(f);
            return level = core::CrafterLevelFromJson(std::string_view(buf, n));
        }

        void PollCraft()
        {
            uint64_t qty[core::kKindCount];
            LocalRoom(qty);
            gCraft.need = LocalCraftLevel() >= 3 ? 2 : 3;   // III: 2 crafts, else II
            const bool inCraftMenu = Alive() &&
                (net::gui2::BackpackOpen() || net::gui2::PlayerMenuActive() == 1);
            const int earned = gCraft.Note(qty, inCraftMenu, RandomKind());
            if (earned >= 0)
                NoteCrafted(earned);   // owns the ExtLoaded() yield

            const bool have = Qty(core::kGiftSid) > 0;
            if (have && !gHadGift && !core::ValidKind(gKind) && NativeOwner())
                NoteGiftKind(RandomKind());
            gHadGift = have;
        }

        bool PeerGiftable(int slot)
        {
            if (slot < 0 || slot >= kMaxNetPlayers ||
                static_cast<int>(net::session::MySlot()) == slot)
                return false;
            const NetPlayer& p = NetPlayers::BySlot(slot);
            return p.IsActiveRemote() && p.spawned && !p.dead && !p.dying &&
                   p.npcSid && NetPlayers::SlotIsFriendly(slot) &&
                   net::match::downed::PeerState(static_cast<uint8_t>(slot)) != net::match::downed::kStateDown;
        }

        void BroadcastStock(const uint64_t now)
        {
            uint64_t qty[core::kKindCount];
            LocalRoom(qty);
            const uint8_t mask = core::RoomMask(qty);
            if (mask == gLastMask && now - gLastStock < core::kStockBroadcastMs)
                return;
            SendRaw(static_cast<uint8_t>(NEVT_GIFT_STOCK), &mask, 1);
            gLastMask = mask;
            gLastStock = now;
        }

        void FeedPrompt(const uint64_t now)
        {
            const bool carried = core::ValidKind(gKind) && Qty(core::kGiftSid) > 0;
            net::ui::prompt_producer::NoteGiftCarried(carried ? gKind : -1);
            for (int slot = 0; slot < kMaxNetPlayers; ++slot) {
                const bool known = core::Fresh(now, gRoomAt[slot]);
                net::ui::prompt_producer::SetGiftPeerRoom(
                    slot, known, known ? gRoom[slot] : static_cast<uint8_t>(0));
            }
        }

        void Give(const uint64_t now, int slot)
        {
            const uint8_t kind = static_cast<uint8_t>(gKind);
            SendTo(static_cast<uint8_t>(slot),
                   static_cast<uint8_t>(NEVT_GIFT), &kind, 1);
            gPending = slot;
            gPendingKind = gKind;
            gPendingUntil = now + core::kPendingTimeoutMs;
            gAck.store(0);
            TO1_LOG("[gift] giving %s to slot=%d", KindName(gKind), slot);
        }

        void ResolvePending(const uint64_t now)
        {
            if (gPending < 0)
                return;
            const int answer = gAck.exchange(0);
            if (!answer) {
                if (now > gPendingUntil) {
                    TO1_LOG("[gift] no answer from slot=%d - the gift stays", gPending);
                    gPending = -1;
                    gPendingKind = -1;
                }
                return;
            }

            const int slot = gPending;
            const int kind = gPendingKind;
            gPending = -1;
            gPendingKind = -1;
            char text[160];

            if (answer == 2) {
                if (slot >= 0 && slot < kMaxNetPlayers && core::ValidKind(kind)) {
                    gRoom[slot] &= static_cast<uint8_t>(~(1u << kind));
                    gRoomAt[slot] = now;
                }
                std::snprintf(text, sizeof text, "Gift: %s can't carry any more %ss",
                              slot >= 0 && slot < kMaxNetPlayers
                                  ? NetPlayers::BySlot(slot).name : "teammate",
                              KindName(kind));
                Toast(text, 2.5f);
                return;
            }

            const int receiver = slot;
            RemoveGiftItem();
            gKind = -1;
            std::snprintf(text, sizeof text, "Gift: %s received a %s",
                          receiver >= 0 && receiver < kMaxNetPlayers
                              ? NetPlayers::BySlot(receiver).name : "teammate",
                          KindName(kind));
            Toast(text, 3.0f);

            if (gQueueN > 0) {
                gKind = gQueue[0];
                for (int i = 1; i < gQueueN; ++i)
                    gQueue[i - 1] = gQueue[i];
                --gQueueN;
                Add(core::kGiftSid, 1);
                Toast("Crafter: Your next gift is ready.", 3.0f);
            }

            // 2013 NET_STAT_GAVE_GIFT: flat 100 parts, never boosted by
            // Collector / Increased Parts Earnings.
            const int local = net::session::MySlot();
            if (local >= 0 && local < kMaxNetPlayers)
                net::match::Get().stats[local].parts += 100;
            TO1_LOG("[gift] slot=%d took the %s (+100 parts)", receiver, KindName(kind));
        }

        void TickGiver(const uint64_t now)
        {
            if (!core::ValidKind(gKind) || gPending >= 0 || !Alive() ||
                Qty(core::kGiftSid) == 0 ||
                engine::backpack_prop::CurrentWeapon() != core::kGiftSid)
                return;

            const Transform me = LOCATOR::GET_TRANSFORM(SID("player"));
            int best = -1;
            float bestXZ = core::kGiverXZ * core::kGiverXZ;
            for (const NetPlayer& p : NetPlayers::Remotes()) {
                if (!PeerGiftable(p.netSlot))
                    continue;
                const Transform them = LOCATOR::GET_TRANSFORM(p.npcSid);
                const float dx = them.Position.x - me.Position.x;
                const float dy = them.Position.y - me.Position.y;
                const float dz = them.Position.z - me.Position.z;
                if (!core::GiverReach(dx, dy, dz))
                    continue;
                const float xz = dx * dx + dz * dz;
                if (xz >= bestXZ)
                    continue;
                PollLos(p.netSlot, true);
                if (!LosClear(p.netSlot))
                    continue;
                bestXZ = xz;
                best = p.netSlot;
            }
            if (best < 0)
                return;

            // The label and the give itself both require a fresh stock mask:
            // an unknown / stale peer is never guessed as having room.
            if (!core::Fresh(now, gRoomAt[best]))
                return;
            const bool press = net::ui::prompt_anim::PressEdge();
            if (!press)
                return;
            if (core::HasRoomInMask(gRoom[best], gKind))
                Give(now, best);
            else {
                char text[160];
                std::snprintf(text, sizeof text, "Gift: %s can't carry any more %ss",
                              NetPlayers::BySlot(best).name, KindName(gKind));
                Toast(text, 2.5f);
            }
        }

        void OnStock(uint8_t sender, const uint8_t* data, uint8_t size)
        {
            if (sender >= kMaxNetPlayers || size < 1)
                return;
            gRoom[sender] = static_cast<uint8_t>(data[0] & 0xFu);
            gRoomAt[sender] = ClockMs();
        }

        void OnGive(uint8_t sender, const uint8_t* data, uint8_t size)
        {
            uint8_t answer = 0;
            if (sender >= kMaxNetPlayers || size < 1 || !Alive() ||
                !PeerGiftable(sender))
                return;
            const int kind = data[0];
            if (!core::ValidKind(kind))
                return;
            const uint64_t now = ClockMs();
            if (!core::RateReady(now, gInAt[sender]))
                return;

            const NetPlayer& p = NetPlayers::BySlot(sender);
            const Transform me = LOCATOR::GET_TRANSFORM(SID("player"));
            const float dx = p.stateNew.posX - me.Position.x;
            const float dy = p.stateNew.posY - me.Position.y;
            const float dz = p.stateNew.posZ - me.Position.z;
            if (!core::ReceiverReach(dx, dy, dz))
                return;
            gInAt[sender] = now;

            if (core::HasRoom(Qty(core::kItemSid[kind]), kind)) {
                Add(core::kItemSid[kind], 1);
                gCraft.skipNext = true;   // a gift is not a completed craft
                char text[160];
                std::snprintf(text, sizeof text, "Gift: %s gave you a %s",
                              p.name, KindName(kind));
                Toast(text, 3.0f);
                answer = 1;
            }
            SendTo(sender, static_cast<uint8_t>(NEVT_GIFT_ACK), &answer, 1);
        }

    } // namespace

    void Install()
    {
        if constexpr (!kEnabled)
            return;
        if (g_installed || boot::ExtLoaded())
            return;
        g_installed = true;
        TO1_LOG("[gift] native bridge installed");
    }

    void Tick()
    {
        if (!g_installed || !NativeOwner())
            return;
        const uint64_t now = ClockMs();
        PollCraft();
        BroadcastStock(now);
        ResolvePending(now);

        if (core::ValidKind(gKind) && gPending < 0 && Alive() &&
            Qty(core::kGiftSid) == 0 && now - gRegrantAt > core::kReceiverRateMs) {
            gRegrantAt = now;
            Add(core::kGiftSid, 1);
        }

        TickGiver(now);
        FeedPrompt(now);
    }

    void OnEvent(uint8_t sender, uint8_t type, const uint8_t* data, uint8_t size)
    {
        if (!g_installed || !NativeOwner() || !data)
            return;
        switch (static_cast<NetEventType>(type)) {
        case NEVT_GIFT_STOCK:
            OnStock(sender, data, size);
            break;
        case NEVT_GIFT:
            OnGive(sender, data, size);
            break;
        case NEVT_GIFT_ACK:
            if (size >= 1 && sender == static_cast<uint8_t>(gPending) &&
                sender < kMaxNetPlayers)
                gAck.store(data[0] ? 1 : 2);
            break;
        default:
            break;
        }
    }

    void Reset()
    {
        gKind = -1;
        gCraft = core::CraftCounter{};
        gHadGift = false;
        gQueue.fill(0);
        gQueueN = 0;
        gLastMask = 0xFF;
        gLastStock = 0;
        gRegrantAt = 0;
        for (int slot = 0; slot < kMaxNetPlayers; ++slot) {
            gRoom[slot] = 0;
            gRoomAt[slot] = 0;
            gInAt[slot] = 0;
        }
        gPending = -1;
        gPendingKind = -1;
        gPendingUntil = 0;
        gAck.store(0);
        gLosJob.fill(0);
        gLosBlocked.fill(false);
        net::ui::prompt_producer::NoteGiftCarried(-1);
        for (int slot = 0; slot < kMaxNetPlayers; ++slot)
            net::ui::prompt_producer::SetGiftPeerRoom(slot, false, 0);
    }

    bool Installed() noexcept
    {
        return g_installed && !boot::ExtLoaded();
    }

    void NoteGiftKind(int kind)
    {
        if (core::ValidKind(kind))
            gKind = kind;
    }

    void NoteCrafted(int kind)
    {
        if (!NativeOwner() || !core::ValidKind(kind))
            return;
        if (core::ValidKind(gKind) && Qty(core::kGiftSid) > 0) {
            if (gQueueN < static_cast<int>(gQueue.size()))
                gQueue[gQueueN++] = static_cast<uint8_t>(kind);
            Toast("Crafter: You crafted another gift! It is ready after you give this one.", 4.0f);
            return;
        }
        gKind = kind;
        Add(core::kGiftSid, 1);
        Toast("Crafter: You crafted a gift! Give it to a teammate for parts.", 4.0f);
    }

} // namespace net::gift
