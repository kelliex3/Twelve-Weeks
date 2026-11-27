// Native pull-out bridge: real clock, shared input, exact family-5 wire and
// authored hits. Every producer here yields while t2r-ext.dll is loaded.

#include "melee-pullout-bridge.hpp"

#include "boot.hpp"
#include "engine/anim-prims.hpp"
#include "engine/frame.hpp"
#include "engine/local-player.hpp"
#include "engine/object.hpp"
#include "engine/pad.hpp"
#include "engine/ptr.hpp"
#include "engine/raycast.hpp"
#include "engine/script.hpp"
#include "engine/window.hpp"
#include "gameplay/melee-duel.hpp"
#include "gameplay/melee-exchange.hpp"
#include "gameplay/melee-pullout-core.hpp"
#include "gameplay/melee.hpp"
#include "log.hpp"
#include "net/match/downed-state.hpp"
#include "net/match/match-liveness.hpp"
#include "net/anim/paired.hpp"
#include "net/audio/pullout-sting.hpp"
#include "net/game/net-hooks.hpp"
#include "net/match/net-player.hpp"
#include "net/online/net-session.hpp"
#include "net/protocol/net-protocol.hpp"
#include "net/ui/prompt-anim.hpp"
#include "net/ui/prompt-producer.hpp"
#include "sid/sid.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>

namespace net::match::melee_pullout_bridge {

    bool g_installed = false;

    namespace {
        namespace core = gameplay::melee_pullout::core;

        constexpr uint32_t kIntroMs = 400;  // no native SP clip: a short tell before the drag

        uint64_t ClockMs()
        {
            const double ms = engine::frame::NowMs();
            return ms > 0.0 ? static_cast<uint64_t>(ms) : 0ull;
        }

        bool NativeOwner()
        {
            return kEnabled && !boot::ExtLoaded() && net::match::liveness::Live();
        }

        bool PeerValid(int slot);   // defined with the target-selection helpers below

        void Log(const char* text)
        {
            if (text)
                TO1_LOG("[pullout-bridge] %s", text);
        }

        enum class AttState : uint8_t { kIdle, kIntro, kDrag, kWaitRes, kOut };

        struct Attacker {
            AttState st = AttState::kIdle;
            int      slot = -1;
            uint8_t  id = 0;
            uint64_t started = 0;
            uint64_t introEnd = 0;
            uint64_t dragEnd = 0;
            uint64_t resAt = 0;
            uint64_t outAt = 0;
            uint64_t outEnd = 0;
            uint64_t blowAt = 0;
            uint8_t  verdict = 0;      // 0 none, 1 counter, 2 failed window
            float    vicYaw = 0.0f;    // victim yaw at the drag hand-off
            bool     camSent = false;
            bool     blowDone = false;
        } gAtt;

        struct Victim {
            bool     on = false;
            bool     decided = false;     // RES already sent; only the linger remains
            int      slot = -1;
            uint8_t  id = 0;
            uint64_t started = 0;
            uint64_t end = 0;
            uint64_t shownAt = 0;
            uint64_t counterHitAt = 0;
            uint64_t outEnd = 0;
            bool     counter = false;
            bool     hitDone = false;
            float    vicYaw = 0.0f;
            core::Window w;
            core::PressState press;
        } gVic;

        // The supine outlives the QTE linger, so its timers live here rather
        // than in Victim: ClearVictim() at 1.75 s must not cut the 4 s watch.
        struct Supine {
            uint64_t checkAt = 0;      // one "did it take?" retry at 400 ms
            uint64_t ovrUntil = 0;     // camera-aim-override-direction lease
            uint64_t aimUntil = 0;     // force-camera-aim-point lease
            uint8_t  tries = 0;
            bool     ovr = false;
        } gSup;

        // SP camera lease. The live camera is sampled once and blended in /
        // out with the ext's smoothstep; releasing the lease simply stops the
        // SET_CAMERA_POSITION writes, which hands the camera back to the engine.
        struct SpCam {
            bool       on = false;
            int        role = 0;       // 1 attacker, 2 victim
            uint64_t   start = 0;
            uint64_t   end = 0;
            uint64_t   outEnd = 0;
            Vector3    basePos{};
            Quaternion baseRot{};
        } gSp;

        // Deterministic root pair. The private psync apReference data is not
        // in this build, so both screens agree on the victim's root + yaw and
        // the ctxdown clips pose the two halves; the write happens after the
        // replication tick so the smoother cannot undo it in the same frame.
        struct Pair {
            bool     on = false;
            int      role = 0;      // 1 we are the attacker, 2 we are the victim
            int      slot = -1;
            uint8_t  id = 0;
            uint64_t uid = 0;
            bool     ellie = false;
            bool     outcome = false;
            float    vicYaw = 0.0f;
        } gPair;

        uint8_t                                    gNextId = 1;
        bool                                       gMeleeLatch = false;
        std::array<uint64_t, kMaxNetPlayers>       gCool{};
        std::array<uint64_t, kMaxNetPlayers>       gLosJob{};
        std::array<bool,      kMaxNetPlayers>      gLosBlocked{};

        void SendMex(uint8_t slot, uint8_t op, uint8_t id,
                     const uint8_t* payload, uint8_t len)
        {
            if (!NativeOwner() || slot >= kMaxNetPlayers)
                return;
            uint8_t body[16] = { core::kFamily, op, id, 0 };
            if (payload && len) {
                if (len > 12) len = 12;
                std::memcpy(body + 4, payload, len);
            }
            else len = 0;
            net::session::SendEventRaw(static_cast<uint8_t>(NEVT_MEX), body,
                                       static_cast<uint8_t>(4 + len), slot);
        }

        void SendResult(uint8_t slot, uint8_t id, bool ok, uint16_t relMs)
        {
            const uint8_t payload[3] = {
                static_cast<uint8_t>(ok ? 1 : 0),
                static_cast<uint8_t>(relMs & 0xFFu),
                static_cast<uint8_t>((relMs >> 8) & 0xFFu)
            };
            SendMex(slot, core::kResult, id, payload, sizeof payload);
        }

        void SendDamage(uint8_t slot, float damage, bool lethal)
        {
            if (!NativeOwner() || slot >= kMaxNetPlayers)
                return;
            PlayerDamageEvent evt{};
            evt.weaponSID = SID("abby-unarmed");
            evt.weaponAmmoSID = 0;
            evt.damage = damage;
            evt.lethalClaim = lethal ? 1 : 0;
            evt.headshot = 0;
            net::session::SendEventRaw(static_cast<uint8_t>(NEVT_TAKE_DAMAGE),
                                       &evt, static_cast<uint8_t>(sizeof evt), slot);
        }

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
                Vector3{ them.Position.x, them.Position.y + 0.6f, them.Position.z },
                engine::raycast::kMaskStaticWorld, SID("player"));
        }

        bool LosClear(int slot) noexcept
        {
            return slot >= 0 && slot < kMaxNetPlayers && !gLosBlocked[slot];
        }

        // +Z-forward camera rotation (the engine camera's forward axis).
        Quaternion QuatYawPitch(float yaw, float pitch) noexcept
        {
            const float hy = std::sin(yaw * 0.5f), wy = std::cos(yaw * 0.5f);
            const float sp = std::sin(pitch * 0.5f), cp = std::cos(pitch * 0.5f);
            return { -wy * sp, hy * cp, hy * sp, wy * cp };
        }

        void StopSpCam()
        {
            gSp = SpCam{};    // no restore write: ceasing writes hands it back
        }

        void BeginSpCam(int role, uint64_t started)
        {
            if (!NativeOwner() || (role != 1 && role != 2) ||
                engine::local_player::Health() <= 0.0f || downed::IsDown())
                return;
            const uintptr_t loc = CAMERA::GET_CAMERA_LOCATOR();
            if (!loc)
                return;
            gSp = SpCam{};
            gSp.on = true;
            gSp.role = role;
            gSp.start = started + core::kCameraMs;
            gSp.end = started + core::kDragMs + core::kOutMs;
            gSp.outEnd = gSp.end + static_cast<uint64_t>(core::kSpHandBackMs);
            gSp.basePos = LOCATOR::GET_LOCATOR_POSITION(loc);
            gSp.baseRot = LOCATOR::GET_LOCATOR_ROTATION(loc);
        }

        void TickSpCam(const uint64_t now)
        {
            if (!gSp.on)
                return;
            if (!NativeOwner() || engine::local_player::Health() <= 0.0f ||
                downed::IsDown()) {
                StopSpCam();
                return;
            }
            if (now >= gSp.outEnd) {
                StopSpCam();
                return;
            }

            const int slot = gSp.role == 1 ? gAtt.slot : gVic.slot;
            if (!PeerValid(slot)) {
                StopSpCam();
                return;
            }
            const NetPlayer& p = NetPlayers::BySlot(slot);
            const Transform meT = LOCATOR::GET_TRANSFORM(SID("player"));
            const Transform themT = LOCATOR::GET_TRANSFORM(p.npcSid);
            const float me[3] = { meT.Position.x, meT.Position.y, meT.Position.z };
            const float them[3] = { themT.Position.x, themT.Position.y, themT.Position.z };
            const core::SpPose pose = core::SpCamera(
                gSp.role,
                now > gSp.start ? static_cast<float>(now - gSp.start) * 0.001f : 0.0f,
                me, them);

            const float inW = core::Smooth01(
                static_cast<float>(now - gSp.start) / core::kSpSwingSecs);
            const float outW = core::Smooth01(
                static_cast<float>(gSp.outEnd - now) / core::kSpHandBackMs);
            const float w = std::min(inW, outW);

            // The pull-out sting is a native g_xcam_fov_mul producer now. On the
            // victim camera its 0.9498 -> 1 ramp owns the lens (70 -> 73.74 deg
            // over 2.7 s), exactly where the ext's ctxdown:pu- tracks consume
            // the global. With the swing sidecar off the procedural pose keeps
            // its own lens.
            const float mul = net::audio::pullout_sting::FovMultiplier();
            const float lens = gSp.role == 2 && mul > 0.0f
                ? net::audio::pullout_sting::core::kGameFov * mul : pose.fov;
            // PS32's measured camera-shake-hit-b curve is replayed alongside the
            // engine clip. Pull-out requests roll=0 / yaw=0, so only the pitch
            // column is added here (negative = view down in QuatYawPitch).
            const float pitch = pose.pitch + (gSp.role == 2
                ? net::audio::pullout_sting::MeasuredShake(now) : 0.0f);
            const Quaternion wantedRot = QuatYawPitch(pose.yaw, pitch);
            const Vector3 wantedPos{ pose.pos[0], pose.pos[1], pose.pos[2] };

            const bool handingBack = now >= gSp.end;
            const Vector3 from = handingBack ? wantedPos : gSp.basePos;
            const Vector3 to = handingBack ? gSp.basePos : wantedPos;
            const Quaternion fromRot = handingBack ? wantedRot : gSp.baseRot;
            const Quaternion toRot = handingBack ? gSp.baseRot : wantedRot;

            const Vector3 finalPos{ from.x + (to.x - from.x) * w,
                                    from.y + (to.y - from.y) * w,
                                    from.z + (to.z - from.z) * w };
            CAMERA::SET_CAMERA_POSITION(
                finalPos, Quaternion::Slerp(fromRot, toRot, w));
            CAMERA::OVERRIDE_FOV(70.0f + (lens - 70.0f) * w);
        }

        // ---- deterministic native pair placement -------------------------------------
        struct PairClips { const char* att; const char* vic; };
        constexpr PairClips kDragClips[2] = {
            { "abby-pullout-prone-bw--npc",   "abby-pullout-prone-bw--abby" },
            { "ellie-pullout-prone-bw--npc",  "ellie-pullout-prone-bw--ellie" }
        };
        constexpr PairClips kOkClips[2] = {
            { "abby-pullout-success--npc",  "abby-pullout-success--abby" },
            { "ellie-pullout-success--npc", "ellie-pullout-success--ellie" }
        };
        constexpr PairClips kFailClips[2] = {
            { "abby-pullout-fail--npc",  "abby-pullout-fail--abby" },
            { "ellie-pullout-fail--npc", "ellie-pullout-fail--ellie" }
        };
        constexpr const char* kDragMove[2] = { "pu-drag-abby", "pu-drag-ellie" };
        constexpr const char* kOkMove[2] = { "pu-ok-abby", "pu-ok-ellie" };
        constexpr const char* kFailMove[2] = { "pu-fail-abby", "pu-fail-ellie" };

        float YawFromQuat(const Quaternion& q) noexcept
        {
            const float den = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
            return std::isfinite(den) ? std::atan2(2.0f * (q.w * q.y + q.x * q.z), den) : 0.0f;
        }

        uint16_t QuantYaw(float yaw) noexcept
        {
            constexpr float kTwoPi = 6.28318531f;
            float a = std::fmod(yaw, kTwoPi);
            if (a < 0.0f) a += kTwoPi;
            return static_cast<uint16_t>(a / kTwoPi * 65535.0f);
        }

        float DequantYaw(uint16_t q) noexcept
        {
            constexpr float kTwoPi = 6.28318531f;
            return static_cast<float>(q) / 65535.0f * kTwoPi - 3.14159265f;
        }

        bool PickDragClips(uint64_t attEnt, uint64_t vicEnt, bool& ellie) noexcept
        {
            for (int i = 0; i < 2; ++i)
                if (engine::anim::Len(attEnt, kDragClips[i].att) > 0.0f &&
                    engine::anim::Len(vicEnt, kDragClips[i].vic) > 0.0f) {
                    ellie = i != 0;
                    return true;
                }
            return false;
        }

        void PlayPair(const PairClips& clips, float blend)
        {
            if (gPair.slot < 0 || gPair.slot >= kMaxNetPlayers)
                return;
            const uint64_t npc = NetPlayers::BySlot(gPair.slot).npcSid;
            const uint64_t attEnt = gPair.role == 1 ? SID("player") : npc;
            const uint64_t vicEnt = gPair.role == 1 ? npc : SID("player");
            engine::anim::AnimateFrom(attEnt, clips.att, blend, 0.0f);
            engine::anim::AnimateFrom(vicEnt, clips.vic, blend, 0.0f);
        }

        void StartPair(int role, int slot, uint8_t id, uint64_t uid, float vicYaw)
        {
            if (!NativeOwner() || !PeerValid(slot))
                return;
            const uint64_t npc = NetPlayers::BySlot(slot).npcSid;
            const uint64_t attEnt = role == 1 ? SID("player") : npc;
            const uint64_t vicEnt = role == 1 ? npc : SID("player");
            bool ellie = false;
            if (!PickDragClips(attEnt, vicEnt, ellie))
                return;                 // clips absent on this build: no fake pair
            gPair = Pair{};
            gPair.on = true;
            gPair.role = role;
            gPair.slot = slot;
            gPair.id = id;
            gPair.uid = uid;
            gPair.ellie = ellie;
            gPair.vicYaw = vicYaw;
            PlayPair(kDragClips[ellie ? 1 : 0], 0.5f);
            const int attSlot = role == 1 ? static_cast<int>(net::session::MySlot()) : slot;
            const int vicSlot = role == 1 ? slot : static_cast<int>(net::session::MySlot());
            if (role == 1)
                net::anim::paired::BeginPulloutAttacker(
                    attSlot, vicSlot, npc, uid, SID(kDragMove[ellie ? 1 : 0]));
            else
                net::anim::paired::BeginPulloutVictim(
                    attSlot, vicSlot, npc, uid, SID(kDragMove[ellie ? 1 : 0]));
        }

        void SwapPair(bool counter)
        {
            if (!gPair.on)
                return;
            const int i = gPair.ellie ? 1 : 0;
            PlayPair(counter ? kOkClips[i] : kFailClips[i], 0.3f);
            gPair.outcome = true;
            gPair.uid++;
            const int attSlot = gPair.role == 1
                ? static_cast<int>(net::session::MySlot()) : gPair.slot;
            const int vicSlot = gPair.role == 1
                ? gPair.slot : static_cast<int>(net::session::MySlot());
            const uint64_t npc = NetPlayers::BySlot(gPair.slot).npcSid;
            const char* move = counter ? kOkMove[i] : kFailMove[i];
            if (gPair.role == 1)
                net::anim::paired::BeginPulloutAttacker(
                    attSlot, vicSlot, npc, gPair.uid, SID(move));
            else
                net::anim::paired::BeginPulloutVictim(
                    attSlot, vicSlot, npc, gPair.uid, SID(move));
        }

        void StopPair(bool aborted)
        {
            if (!gPair.on)
                return;
            if (net::anim::paired::Get().kind == net::anim::paired::kCtxdown)
                net::anim::paired::End(aborted);
            gPair = Pair{};
        }

        void TickPairPlacement()
        {
            if (!gPair.on)
                return;
            if (!NativeOwner() || !PeerValid(gPair.slot) ||
                engine::local_player::Health() <= 0.0f || downed::IsDown()) {
                StopPair(true);
                return;
            }

            float root[3];
            float yaw = gPair.vicYaw;
            if (gPair.role == 2) {
                const Transform me = LOCATOR::GET_TRANSFORM(SID("player"));
                root[0] = me.Position.x;
                root[1] = me.Position.y;
                root[2] = me.Position.z;
                yaw = YawFromQuat(me.Rotation);
            }
            else {
                const NetPlayer& p = NetPlayers::BySlot(gPair.slot);
                root[0] = p.stateNew.posX;
                root[1] = p.stateNew.posY;
                root[2] = p.stateNew.posZ;
            }
            const core::PairPose pose = core::PulloutPairPose(root, yaw);
            const Quaternion facing{
                0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
            const Vector3 attPos{ pose.attPos[0], pose.attPos[1], pose.attPos[2] };
            const Vector3 vicPos{ pose.vicPos[0], pose.vicPos[1], pose.vicPos[2] };
            const uintptr_t npc = NetPlayers::BySlot(gPair.slot).npcSid;

            if (gPair.role == 1) {
                LOCATOR::SET_OBJECT_LOCATOR(
                    SID("player"), MEMORY::ALLOC_BOUND_FRAME(attPos, facing));
                net::hooks::PuppetTeleportScope allow;
                DC::Call<"npc-teleport-to">(
                    npc, MEMORY::ALLOC_BOUND_FRAME(vicPos, facing));
            }
            else {
                LOCATOR::SET_OBJECT_LOCATOR(
                    SID("player"), MEMORY::ALLOC_BOUND_FRAME(vicPos, facing));
                net::hooks::PuppetTeleportScope allow;
                DC::Call<"npc-teleport-to">(
                    npc, MEMORY::ALLOC_BOUND_FRAME(attPos, facing));
            }
        }

        // ---- exact supine presentation (ext melee_pullout.c pulfix) -------------------
        // At the hand-off: send-event go-supine, then the engine's own
        // GoProne(mode 3, 1, 0, -1, 0) on our PlayerStealthController (the MP
        // go-supine handler never runs). Two guarded VA detours keep OUR
        // CharacterProne's supine flag while the camera turns, and the script
        // route holds the camera toward the feet / attacker for the same
        // 650 / 900 / 4000 ms windows as the ext. If this exe build does not
        // match the signatures the script route still runs on its own.
        constexpr uint64_t kSupAimMs = 650;
        constexpr uint64_t kSupOvrMs = 900;
        constexpr uint64_t kSupHoldMs = 1500;
        constexpr uint64_t kSupWatchMs = 4000;
        constexpr uint64_t kSupCheckMs = 400;

        constexpr uint32_t kSetSupineRva = 0xa68270;   // CharacterProne::SetSupine(cp, u8 on)
        constexpr uint8_t  kSetSupineBytes[15] = {
            0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
            0x48, 0x83, 0xec, 0x20 };
        constexpr uint32_t kGoProneRva = 0xcdcf90;     // GoProne(ctrl, int mode, u8,u8,float,u8)
        constexpr uint8_t  kGoProneBytes[16] = {
            0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57,
            0x48, 0x8d, 0x6c, 0x24, 0xf0 };
        constexpr uintptr_t kStealthCtrlOff = 0x31f8;   // Player -> PlayerStealthController
        constexpr uintptr_t kScHandleOff = 0x118;       // -> {ptr, offset, id}
        constexpr uintptr_t kScIdOff = 0x124;
        constexpr uintptr_t kGoProneSlot = 0x1f0;       // vtable slot of GoProne

        SafetyHookInline       g_hkSetSupine;
        SafetyHookInline       g_hkGoProne;
        bool                   g_supHooksTried = false;
        std::atomic<uintptr_t> g_supCp{ 0 };        // our CharacterProne, refreshed every tick
        std::atomic<uintptr_t> g_supCtrl{ 0 };      // our controller, fixed at the hand-off
        std::atomic<uint64_t>  g_supWatch{ 0 };     // GetTickCount64 deadlines for the detours
        std::atomic<uint64_t>  g_supHold{ 0 };
        float                  g_supAim[4]{};      // force-camera-aim-point arg: xyz + valid

        // These run on an engine job thread: relaxed atomics and the trampoline
        // only, no logging or script calls from here.
        void hk_SetSupine(uint8_t* cp, uint8_t on)
        {
            if (!boot::ExtLoaded() && cp) {
                const uint64_t now = ::GetTickCount64();
                const uint64_t until = g_supWatch.load(std::memory_order_relaxed);
                const uintptr_t mine = g_supCp.load(std::memory_order_relaxed);
                if (until && now < until && mine && (uintptr_t)cp == mine &&
                    now < g_supHold.load(std::memory_order_relaxed) && !on)
                    on = 1;     // the look rule's clear must not roll us onto the belly
            }
            g_hkSetSupine.call<void>(cp, on);
        }

        void hk_GoProne(uint8_t* ctrl, int mode, uint8_t a, uint8_t b,
                        float blend, uint8_t c)
        {
            if (!boot::ExtLoaded() && ctrl &&
                (uintptr_t)ctrl == g_supCtrl.load(std::memory_order_relaxed)) {
                const uint64_t now = ::GetTickCount64();
                const uint64_t until = g_supWatch.load(std::memory_order_relaxed);
                if (until && now < until && mode == 1 &&
                    now < g_supHold.load(std::memory_order_relaxed))
                    mode = 3;   // mode 1 = s_prone (belly); 3 = s_prone-supine
            }
            g_hkGoProne.call<void>(ctrl, mode, a, b, blend, c);
        }

        bool ExeBytesAt(uint32_t rva, const uint8_t* expect, size_t n)
        {
            const uintptr_t at = g_base + rva;
            return engine::ptr::Readable(at, n) &&
                   !std::memcmp(reinterpret_cast<const void*>(at), expect, n);
        }

        void InstallSupHooks()
        {
            if (g_supHooksTried)
                return;
            g_supHooksTried = true;
            if (boot::ExtLoaded())
                return;                 // the ext owns these RVAs while it is loaded
            if (ExeBytesAt(kSetSupineRva, kSetSupineBytes, sizeof kSetSupineBytes))
                g_hkSetSupine = safetyhook::create_inline(
                    reinterpret_cast<void*>(g_base + kSetSupineRva),
                    reinterpret_cast<void*>(hk_SetSupine));
            else
                Log("exe signature mismatch at 0xa68270; supine flag hook unavailable");
            if (ExeBytesAt(kGoProneRva, kGoProneBytes, sizeof kGoProneBytes))
                g_hkGoProne = safetyhook::create_inline(
                    reinterpret_cast<void*>(g_base + kGoProneRva),
                    reinterpret_cast<void*>(hk_GoProne));
            else
                Log("exe signature mismatch at 0xcdcf90; GoProne hook unavailable");
        }

        // Player +0x31f8 = PlayerStealthController; its +0x118 handle
        // {ptr, +0x120 offset, +0x124 id} walks to the subsystem whose id must
        // match +0x78 (the same walk GoProne does). 0 = not prone.
        uintptr_t MyCprone()
        {
            uint8_t* pl = GameFn<uint8_t* (*)()>(0xdabea0)();
            if (!pl || !engine::ptr::Readable(
                    reinterpret_cast<uintptr_t>(pl) + kStealthCtrlOff, 8))
                return 0;
            uint8_t* sc = *reinterpret_cast<uint8_t**>(pl + kStealthCtrlOff);
            if (!sc || !engine::ptr::Readable(
                    reinterpret_cast<uintptr_t>(sc) + kScHandleOff, 16))
                return 0;
            uint8_t* h = *reinterpret_cast<uint8_t**>(sc + kScHandleOff);
            const uint32_t off = *reinterpret_cast<uint32_t*>(sc + kScHandleOff + 8);
            const uint32_t id = *reinterpret_cast<uint32_t*>(sc + kScIdOff);
            if (!h || !id || !engine::ptr::Readable(
                    reinterpret_cast<uintptr_t>(h) + 8, 8))
                return 0;
            uint8_t* b = *reinterpret_cast<uint8_t**>(h + 8);
            if (!b)
                return 0;
            uint8_t* cp = b + off;
            if (!engine::ptr::Readable(reinterpret_cast<uintptr_t>(cp), 0x1f0) ||
                *reinterpret_cast<uint32_t*>(cp + 0x78) != id)
                return 0;
            return reinterpret_cast<uintptr_t>(cp);
        }

        void CallGoProne(uint8_t* ctrl, int mode, uint8_t a, uint8_t b,
                         float blend, uint8_t c)
        {
            if (!ctrl || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(ctrl), 8))
                return;
            uint8_t* vt = *reinterpret_cast<uint8_t**>(ctrl);
            if (!vt || !engine::ptr::Readable(
                    reinterpret_cast<uintptr_t>(vt) + kGoProneSlot, 8))
                return;
            using Fn = void (*)(uint8_t*, int, uint8_t, uint8_t, float, uint8_t);
            const Fn fn = *reinterpret_cast<Fn*>(vt + kGoProneSlot);
            if (fn)
                fn(ctrl, mode, a, b, blend, c);
        }

        // A point down along the body's feet, moved toward the attacker but
        // clamped to +-60 deg of the feet (the engine holds supine within
        // +-67 deg). This is what keeps the camera from looking over the head.
        void ComputeSupAim(int slot)
        {
            g_supAim[3] = 0.0f;
            float pelvis[3], neck[3];
            if (!engine::object::JointLocator(SID("player"), "pelvis", pelvis, nullptr) ||
                !engine::object::JointLocator(SID("player"), "neck", neck, nullptr))
                return;
            const float x = pelvis[0] - neck[0], z = pelvis[2] - neck[2];
            const float n = std::sqrt(x * x + z * z);
            if (!(n > 0.15f))
                return;                 // sitting up / degenerate: no lying axis
            const float fx = x / n, fz = z / n;
            g_supAim[0] = pelvis[0] + fx * 4.0f;
            g_supAim[1] = pelvis[1] + 0.25f;
            g_supAim[2] = pelvis[2] + fz * 4.0f;
            g_supAim[3] = 1.0f;

            if (!PeerValid(slot))
                return;
            const NetPlayer& peer = NetPlayers::BySlot(slot);
            const float ax = peer.stateNew.posX - pelvis[0];
            const float az = peer.stateNew.posZ - pelvis[2];
            const float ad = std::sqrt(ax * ax + az * az);
            if (!std::isfinite(ad) || ad <= 0.5f || ad >= 15.0f)
                return;
            const float rel = std::atan2(fz * ax - fx * az, fx * ax + fz * az);
            constexpr float lim = 60.0f * 0.0174532925f;
            const float cl = std::max(-lim, std::min(lim, rel));
            const float th0 = std::atan2(fx, fz), th1 = th0 - cl, th2 = th0 + cl;
            const float d1 = std::sin(th1) * ax + std::cos(th1) * az;
            const float d2 = std::sin(th2) * ax + std::cos(th2) * az;
            const float th = d1 >= d2 ? th1 : th2;
            const float dd = ad < 2.0f ? 2.0f : ad;
            g_supAim[0] = pelvis[0] + std::sin(th) * dd;
            g_supAim[1] = peer.stateNew.posY + 1.1f;
            g_supAim[2] = pelvis[2] + std::cos(th) * dd;
        }

        void CancelSupine()
        {
            if (gSup.ovr) {
                DC::Call<"camera-cancel-aim-override">();
                gSup.ovr = false;
            }
            gSup = Supine{};
            g_supWatch.store(0, std::memory_order_relaxed);
            g_supHold.store(0, std::memory_order_relaxed);
            g_supCp.store(0, std::memory_order_relaxed);
            g_supCtrl.store(0, std::memory_order_relaxed);
            g_supAim[3] = 0.0f;
        }

        void BeginSupine(int slot)
        {
            if (!NativeOwner() || !PeerValid(slot))
                return;
            CancelSupine();
            InstallSupHooks();
            const uint64_t now = ::GetTickCount64();
            gSup.tries = 1;
            gSup.checkAt = now + kSupCheckMs;
            gSup.ovrUntil = now + kSupOvrMs;
            gSup.aimUntil = now + kSupAimMs;
            g_supWatch.store(now + kSupWatchMs, std::memory_order_relaxed);
            g_supHold.store(now + kSupHoldMs, std::memory_order_relaxed);
            ComputeSupAim(slot);

            uint8_t* pl = GameFn<uint8_t* (*)()>(0xdabea0)();
            uint8_t* ctrl = nullptr;
            if (pl && engine::ptr::Readable(
                    reinterpret_cast<uintptr_t>(pl) + kStealthCtrlOff, 8))
                ctrl = *reinterpret_cast<uint8_t**>(pl + kStealthCtrlOff);
            g_supCtrl.store(reinterpret_cast<uintptr_t>(ctrl),
                            std::memory_order_relaxed);
            DC::Call<"send-event">(SID("go-supine"), SID("player"));

            // MP never runs the go-supine handler, so call its GoProne ourselves
            // when there is no top CharacterProne yet (mode 3 = supine spawn).
            if (!MyCprone())
                CallGoProne(ctrl, 3, 1, 0, -1.0f, 0);
            g_supCp.store(MyCprone(), std::memory_order_relaxed);
            gSup.ovr = DcManager::Lookup(SID("camera-aim-override-direction")) != nullptr;
        }

        void TickSupine()
        {
            if (!g_supWatch.load(std::memory_order_relaxed))
                return;
            const uint64_t wall = ::GetTickCount64();
            if (wall >= g_supWatch.load(std::memory_order_relaxed)) {
                CancelSupine();
                return;
            }

            // A newly created CharacterProne starts from its spawn mode; track
            // it and restart the hold exactly like the ext's watch does.
            const uintptr_t cp = MyCprone();
            const uintptr_t prev = g_supCp.load(std::memory_order_relaxed);
            if (cp != prev) {
                if (cp && prev && wall < g_supHold.load(std::memory_order_relaxed))
                    g_supHold.store(wall + kSupHoldMs, std::memory_order_relaxed);
                g_supCp.store(cp, std::memory_order_relaxed);
            }
            // Drive the flag through the engine's own SetSupine: it writes the
            // +0x1e1 byte AND switches the overlay set to "supine".
            static uint64_t lastSetAt = 0;
            static uintptr_t lastSetCp = 0;
            if (cp && g_hkSetSupine && wall <
                g_supHold.load(std::memory_order_relaxed)) {
                uint8_t* c = reinterpret_cast<uint8_t*>(cp);
                if (!c[0x1e1] || cp != lastSetCp || wall - lastSetAt >= 100) {
                    lastSetAt = wall;
                    lastSetCp = cp;
                    g_hkSetSupine.call<void>(c, 1);
                }
            }

            const uint64_t aimRaw = DC::Call<"player-is-aiming?">();
            const bool aiming = aimRaw != ~0ULL && (aimRaw & 1u) != 0;
            if (gSup.ovr && (wall >= gSup.ovrUntil || aiming)) {
                DC::Call<"camera-cancel-aim-override">();
                gSup.ovr = false;
            }
            if (g_supAim[3] == 1.0f && wall >= gSup.aimUntil)
                g_supAim[3] = 0.0f;
            if (g_supAim[3] == 1.0f && aiming)
                g_supAim[3] = 0.0f;      // the player's own aim wins
            if (gSup.ovr && g_supAim[3] == 1.0f) {
                float head[3];
                if (engine::object::JointLocator(SID("player"), "headb", head, nullptr)) {
                    const float dx = g_supAim[0] - head[0];
                    const float dy = g_supAim[1] - head[1];
                    const float dz = g_supAim[2] - head[2];
                    const float flat = std::sqrt(dx * dx + dz * dz);
                    const float yaw = std::atan2(dx, dz) * 57.29578f;
                    const float pitch = std::atan2(dy, flat > 1e-3f ? flat : 1e-3f) *
                                        57.29578f;
                    // Variant 2 of the ext's calibrated override: (-yaw, pitch).
                    DC::Call<"camera-aim-override-direction">(-yaw, pitch);
                }
            }
            if (g_supAim[3] == 1.0f &&
                DcManager::Lookup(SID("force-camera-aim-point")))
                DC::Call<"force-camera-aim-point">(
                    reinterpret_cast<uintptr_t>(g_supAim));

            if (!gSup.checkAt || wall < gSup.checkAt)
                return;
            gSup.checkAt = 0;
            const uint64_t supine = DC::Call<"player-is-supine?">();
            if ((supine == ~0ULL || !(supine & 1u)) && gSup.tries < 2) {
                ++gSup.tries;
                DC::Call<"send-event">(SID("go-supine"), SID("player"));
            }
        }

        bool LocalFree()
        {
            return engine::local_player::Health() > 0.0f &&
                   !downed::IsDown() &&
                   gameplay::melee_exchange::CurrentRole() == gameplay::melee_exchange::kIdle &&
                   !gameplay::melee_duel::Active() &&
                   !gameplay::melee::CommittedToExecutor() &&
                   !(net::anim::paired::Active() &&
                     net::anim::paired::Get().kind != net::anim::paired::kCtxdown) &&
                   !DC::Call<"player-is-prone?">() &&
                   !DC::Call<"player-is-crouched?">();
        }

        bool PeerValid(int slot)
        {
            if (slot < 0 || slot >= kMaxNetPlayers ||
                static_cast<int>(net::session::MySlot()) == slot)
                return false;
            const NetPlayer& p = NetPlayers::BySlot(slot);
            return p.IsActiveRemote() && p.spawned && !p.dead && !p.dying &&
                   p.npcSid && !p.EngineOwned() &&
                   downed::PeerState(static_cast<uint8_t>(slot)) != downed::kStateDown;
        }

        // The pull-out sting's DOF call needs a LIVE puppet handle: the ext
        // crashed when force-camera-dof-focus was handed a StringId whose
        // process had already gone. Validate the same object route the engine
        // uses (ENTITY::DOES_EXIST + LookupObject) at the grab edge; the owner
        // re-validates every DOF frame in case the puppet respawns mid-drag.
        uint64_t VerifiedPuppetHandle(int slot)
        {
            if (!PeerValid(slot))
                return 0;
            const NetPlayer& p = NetPlayers::BySlot(slot);
            const auto sid = static_cast<StringId64>(p.npcSid);
            return ENTITY::DOES_EXIST(sid) && DcManager::LookupObject(sid)
                 ? static_cast<uint64_t>(p.npcSid) : 0;
        }

        float YawFromState(const PlayerNetState& s)
        {
            const float den = 1.0f - 2.0f * (s.rotY * s.rotY + s.rotZ * s.rotZ);
            if (!std::isfinite(den))
                return 0.0f;
            return std::atan2(2.0f * (s.rotW * s.rotY + s.rotX * s.rotZ), den);
        }

        // Nearest standing enemy inside the ext's 2.5 m / 55 deg behind cone.
        // A cover/path detour is not ported; the static-world ray is the only
        // conservative gate, and an unanswered ray stays optimistic.
        int FindTarget()
        {
            const Transform me = LOCATOR::GET_TRANSFORM(SID("player"));
            const uint64_t now = ClockMs();
            int best = -1;
            float bestDist = core::kRange * core::kRange;
            for (const NetPlayer& p : NetPlayers::Remotes()) {
                const int slot = p.netSlot;
                if (!PeerValid(slot) || NetPlayers::SlotIsFriendly(slot) ||
                    !core::CooldownReady(now, gCool[slot]))
                    continue;
                const float dx = p.stateNew.posX - me.Position.x;
                const float dz = p.stateNew.posZ - me.Position.z;
                const float dy = (p.stateNew.posY + core::kProneHead) -
                                 (me.Position.y + 1.0f);
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (!std::isfinite(d2) || d2 >= bestDist)
                    continue;
                const float yaw = YawFromState(p.stateNew);
                const float backX = -std::sin(yaw), backZ = -std::cos(yaw);
                if (!core::BehindCone(me.Position.x, me.Position.z,
                                      p.stateNew.posX, p.stateNew.posZ,
                                      backX, backZ, false))
                    continue;
                PollLos(slot, true);
                if (!LosClear(slot))
                    continue;
                bestDist = d2;
                best = slot;
            }
            return best;
        }

        void ClearVictim(bool notify)
        {
            if (!gVic.on && !gVic.decided)
                return;
            const int peer = gVic.slot;
            const uint8_t id = gVic.id;
            net::audio::pullout_sting::OnTick(ClockMs(), false, false, true);
            gVic = Victim{};   // the supine watch outlives this QTE linger
            PollLos(peer, false);
            if (gSp.role == 2)
                StopSpCam();
            if (gPair.role == 2)
                StopPair(false);
            if (notify && peer >= 0 && peer < kMaxNetPlayers)
                SendMex(static_cast<uint8_t>(peer), core::kEnd, id, nullptr, 0);
        }

        void EndAttack(const char* why, bool notify)
        {
            if (gAtt.st == AttState::kIdle)
                return;
            const int slot = gAtt.slot;
            const uint8_t id = gAtt.id;
            const uint64_t now = ClockMs();
            const bool fail = gAtt.verdict != 1;
            if (notify && slot >= 0 && slot < kMaxNetPlayers)
                SendMex(static_cast<uint8_t>(slot), core::kEnd, id, nullptr, 0);
            if (slot >= 0 && slot < kMaxNetPlayers)
                gCool[slot] = now + (fail ? core::kCoolFailMs : core::kCoolOkMs);
            if (why)
                TO1_LOG("[pullout-bridge] END (%s) slot=%d verdict=%u", why, slot, gAtt.verdict);
            gAtt = Attacker{};
            PollLos(slot, false);
            if (gSp.role == 1)
                StopSpCam();
            if (gPair.role == 1)
                StopPair(false);
        }

        void BeginAttack(const uint64_t now, int slot)
        {
            const uint8_t bits = core::IntroBits(false, 0, false, false, 0);
            gAtt = Attacker{};
            gAtt.st = AttState::kIntro;
            gAtt.slot = slot;
            gAtt.id = ++gNextId;
            gAtt.started = now;
            gAtt.introEnd = now + kIntroMs;
            gAtt.vicYaw = YawFromState(NetPlayers::BySlot(slot).stateNew);
            const uint16_t yawQ = QuantYaw(gAtt.vicYaw);
            const uint8_t intro[3] = {
                bits, static_cast<uint8_t>(yawQ & 0xFFu),
                static_cast<uint8_t>((yawQ >> 8) & 0xFFu)
            };
            SendMex(static_cast<uint8_t>(slot), core::kIntro, gAtt.id,
                    intro, sizeof intro);
            TO1_LOG("[pullout-bridge] INTRO to slot=%d id=%u", slot, gAtt.id);
        }

        void StartOutcome(const uint64_t now)
        {
            gAtt.st = AttState::kOut;
            gAtt.outAt = now;
            gAtt.outEnd = now + core::kOutMs;
            gAtt.blowAt = now + core::kBlowMs;
            gAtt.blowDone = false;
            SwapPair(gAtt.verdict == 1);
            TO1_LOG("[pullout-bridge] OUTCOME slot=%d %s", gAtt.slot,
                    gAtt.verdict == 1 ? "countered" : "downed");
        }

        void TickAttacker(const uint64_t now, bool trigger)
        {
            if (gAtt.st == AttState::kIdle) {
                if (!trigger || gVic.on || gVic.decided || !LocalFree())
                    return;
                const int slot = FindTarget();
                if (slot >= 0)
                    BeginAttack(now, slot);
                return;
            }

            const int slot = gAtt.slot;
            if (!PeerValid(slot) || !LocalFree()) {
                EndAttack("the victim is gone / we are busy", true);
                return;
            }

            switch (gAtt.st) {
            case AttState::kIntro:
                if (now >= gAtt.introEnd) {
                    gAtt.st = AttState::kDrag;
                    gAtt.dragEnd = now + core::kDragMs;
                    StartPair(1, slot, gAtt.id,
                              (static_cast<uint64_t>(net::session::MySlot()) << 24) | gAtt.id,
                              gAtt.vicYaw);
                    TO1_LOG("[pullout-bridge] DRAG slot=%d hand-off in %u ms",
                            slot, core::kDragMs);
                }
                break;
            case AttState::kDrag:
                if (!gAtt.camSent && now >= gAtt.started + core::kCameraMs) {
                    const uint8_t cam = static_cast<uint8_t>(core::kSpCamIndex);
                    SendMex(static_cast<uint8_t>(slot), core::kCamera, gAtt.id, &cam, 1);
                    BeginSpCam(1, gAtt.started);
                    gAtt.camSent = true;
                }
                if (now >= gAtt.dragEnd) {
                    gAtt.st = AttState::kWaitRes;
                    gAtt.resAt = now;
                }
                break;
            case AttState::kWaitRes:
                if (gAtt.verdict || now - gAtt.resAt >= core::kResultWaitMs)
                    StartOutcome(now);
                break;
            case AttState::kOut:
                if (!gAtt.blowDone && gAtt.verdict != 1 && now >= gAtt.blowAt) {
                    gAtt.blowDone = true;
                    const NetPlayer& p = NetPlayers::BySlot(slot);
                    const float netHp = p.stateNew.health;
                    SendDamage(static_cast<uint8_t>(slot),
                               core::FailDamage(netHp), true);
                }
                if (now >= gAtt.outEnd)
                    EndAttack(gAtt.verdict == 1 ? "countered" : "the victim was downed", true);
                break;
            case AttState::kIdle:
                break;
            }
        }

        void FeedPrompt()
        {
            if (!gVic.on && !gVic.decided) {
                net::ui::prompt_producer::NotePullout(-1, false, 0, 0);
                return;
            }
            net::ui::prompt_producer::NotePullout(
                gVic.slot, gVic.on, gVic.w.open, gVic.end);
        }

        void OnIntro(uint8_t sender, const uint8_t* body, uint8_t size)
        {
            if (size < 5 || gVic.on || gVic.decided)
                return;
            const int local = net::session::MySlot();
            if (local < 0 || sender >= kMaxNetPlayers ||
                sender == static_cast<uint8_t>(local) ||
                downed::IsDown() || engine::local_player::Health() <= 0.0f)
                return;
            const core::Bits bits = core::DecodeIntro(body[4]);
            // Pounce / variant-a and grapple paths have no native pair/placement
            // bridge: accepting their window would fake a sequence we cannot play.
            if (bits.pounce || bits.grapple || bits.variant != 0)
                return;
            const uint64_t now = ClockMs();
            gVic = Victim{};
            gVic.on = true;
            gVic.slot = sender;
            gVic.id = body[2];
            gVic.started = now;
            gVic.end = now + core::kDragMs;
            gVic.shownAt = now;
            gVic.w = core::CounterWindow(gVic.end, gVic.shownAt);
            gVic.vicYaw = size >= 7
                ? DequantYaw(static_cast<uint16_t>(body[5] | (body[6] << 8)))
                : YawFromQuat(LOCATOR::GET_TRANSFORM(SID("player")).Rotation);
            StartPair(2, gVic.slot, gVic.id,
                      (static_cast<uint64_t>(sender) << 24) | gVic.id,
                      gVic.vicYaw);
            net::audio::pullout_sting::OnGrab(
                now, gVic.slot, VerifiedPuppetHandle(gVic.slot));
            TO1_LOG("[pullout-bridge] INTRO from slot=%u id=%u window %llu..%llu",
                    sender, gVic.id,
                    static_cast<unsigned long long>(gVic.w.open),
                    static_cast<unsigned long long>(gVic.w.close));
        }

        void OnResult(uint8_t sender, const uint8_t* body, uint8_t size)
        {
            if (gAtt.st == AttState::kIdle || gAtt.slot < 0 ||
                gAtt.slot != static_cast<int>(sender))
                return;
            if (size < 5 || body[2] != gAtt.id)
                return;
            gAtt.verdict = body[4] ? 1 : 2;
            TO1_LOG("[pullout-bridge] RES from slot=%u ok=%u rel=%u",
                    sender, body[4] ? 1u : 0u,
                    size >= 7 ? static_cast<unsigned>(body[5] | (body[6] << 8)) : 0u);
        }

        void TickVictim(const uint64_t now, bool press)
        {
            if (!gVic.on && !gVic.decided)
                return;
            const int slot = gVic.slot;
            if ((gVic.on && !PeerValid(slot)) || downed::IsDown() ||
                engine::local_player::Health() <= 0.0f) {
                if (gVic.on) {
                    SendResult(static_cast<uint8_t>(slot), gVic.id, false,
                               static_cast<uint16_t>(gVic.press.relMs));
                }
                CancelSupine();
                ClearVictim(false);
                return;
            }

            if (gVic.on) {
                const core::PressResult r = core::Step(
                    now, gVic.w, press, now >= gVic.w.close, gVic.press);
                if (r == core::PressResult::kBurn) {
                    TO1_LOG("[pullout-bridge] counter pressed too early (burnt)");
                }
                else if (r == core::PressResult::kCounter ||
                         r == core::PressResult::kFail) {
                    const bool ok = r == core::PressResult::kCounter;
                    SendResult(static_cast<uint8_t>(slot), gVic.id, ok,
                               gVic.press.relMs);
                    gVic.on = false;
                    gVic.decided = true;
                    gVic.counter = ok;
                    gVic.outEnd = now + core::kOutMs;
                    gVic.counterHitAt = ok ? now + core::kBlowMs : 0;
                    SwapPair(ok);
                    if (ok)
                        BeginSupine(slot);
                    net::audio::pullout_sting::OnVerdict(now, ok);
                    // No victim-side fail-blow timestamp is replicated: use the
                    // bridge's authored kBlowMs (the attacker applies damage then).
                    net::audio::pullout_sting::OnOutcome(
                        now, ok, static_cast<float>(core::kBlowMs) / 1000.0f);
                    TO1_LOG("[pullout-bridge] RES %s slot=%d rel=%u",
                            ok ? "COUNTER" : "FAIL", slot, gVic.press.relMs);
                }
            }

            if (gVic.counter && !gVic.hitDone && gVic.counterHitAt &&
                now >= gVic.counterHitAt) {
                gVic.hitDone = true;
                SendDamage(static_cast<uint8_t>(slot), core::kCounterDamage, false);
            }
            if (gVic.decided && now >= gVic.outEnd)
                ClearVictim(false);
        }

    } // namespace

    void Install()
    {
        if constexpr (!kEnabled)
            return;
        if (g_installed || boot::ExtLoaded())
            return;
        g_installed = true;
        net::audio::pullout_sting::Reset();
        Log("native pull-out bridge installed");
    }

    void Tick()
    {
        // The shared F / melee latch is kept current even while the ext owns the
        // feature, so unloading it mid-session cannot synthesize a stale press.
        const bool raw = engine::window::KeyDown('F') || engine::pad::MeleeHeld();
        const bool trigger = raw && !gMeleeLatch;
        gMeleeLatch = raw;

        if (!g_installed || !NativeOwner())
            return;
        const uint64_t now = ClockMs();
        TickVictim(now, net::ui::prompt_anim::PressEdge());
        TickSupine();
        TickAttacker(now, trigger);
        net::audio::pullout_sting::OnTick(
            now, gVic.on || gVic.decided,
            downed::IsDown() || engine::local_player::Health() <= 0.0f,
            net::match::liveness::Live());
        FeedPrompt();
    }

    void PostNetTick()
    {
        // Camera and pair placement are written after NetTick so replication /
        // spectate cannot overwrite the same frame's cinematic writes.
        if (!g_installed || !NativeOwner())
            return;
        const uint64_t now = ClockMs();
        TickSpCam(now);
        TickPairPlacement();
    }

    void OnEvent(uint8_t sender, const uint8_t* body, uint8_t size)
    {
        if (!g_installed || !NativeOwner() || !body || size < 4 ||
            body[0] != core::kFamily)
            return;
        switch (body[1]) {
        case core::kIntro:
            OnIntro(sender, body, size);
            break;
        case core::kResult:
            OnResult(sender, body, size);
            break;
        case core::kCamera:
            if (size >= 5 && body[4] != 0 && gVic.on &&
                gVic.slot == static_cast<int>(sender))
                BeginSpCam(2, gVic.started);
            break;
        case core::kEnd:
            if (gAtt.slot == static_cast<int>(sender))
                EndAttack("the partner ended it", false);
            else if (gVic.on && gVic.slot == static_cast<int>(sender))
                ClearVictim(false);
            break;
        default:
            break;
        }
    }

    void Reset()
    {
        if (!g_installed)
            return;
        gAtt = Attacker{};
        net::audio::pullout_sting::Reset();
        CancelSupine();
        gVic = Victim{};
        StopSpCam();
        StopPair(true);
        gNextId = 1;
        gMeleeLatch = false;
        gCool.fill(0);
        gLosJob.fill(0);
        gLosBlocked.fill(false);
        net::ui::prompt_producer::NotePullout(-1, false, 0, 0);
    }

    bool Installed() noexcept
    {
        return g_installed && !boot::ExtLoaded();
    }

} // namespace net::match::melee_pullout_bridge
