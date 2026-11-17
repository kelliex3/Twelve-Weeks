#include "barrel-check.hpp"
#include "engine/script.hpp"
#include "engine/draw.hpp"
#include "engine/debugdraw.hpp"
#include "engine/meshray.hpp"
#include "sid/sid.hpp"
#include "sid/sidbase.hpp"
#include "net-gui2.hpp"
#include "log.hpp"

#include <safetyhook.hpp>

#include <atomic>
#include <bit>
#include <cmath>
#include <intrin.h>
#include <iterator>
#include <mutex>

namespace gameplay::barrelcheck {

    bool g_enabled  = true;
    bool g_drawDebug = false;

    float g_rHandVOffset = 0.07f;
    float g_rHandHOffset = 0.00f;

    namespace {

        constexpr uint64_t kRaycastMask    = 6919;
        constexpr float    kCamRayLength   = 1000.0f;
        constexpr float    kAlignTolerance = 0.5f;

        constexpr uint32_t kColBlocked = _rgb(255, 64, 64);
        constexpr uint32_t kColClear   = _rgb(96, 255, 128);
        constexpr uint32_t kColTarget  = _rgb(255, 224, 96);

        constexpr StringId64 kImpactSquib = SID("squib-metal-rifle");
        constexpr int        kMaxFxPerTick = 4;

        std::atomic<bool> s_blocked{ false };
        std::atomic<int>  s_pendingFx{ 0 };

        uint64_t s_camRayId    = 0;
        uint64_t s_barrelRayId = 0;
        bool     s_barrelPending = false;
        Vector3  s_barrelFrom;
        Vector3  s_barrelTarget;
        uint64_t s_targetObj = 0;

        bool    s_lastBlockValid = false;
        Vector3 s_lastBlockPos;
        Vector3 s_lastBlockDir;

        constexpr float kXSmoothTau = 0.12f;
        constexpr float kXSnapDist  = 1.5f;
        constexpr float kXHoldTime  = 0.15f;

        constexpr float kMinBlockDistanceDrawMarker = 5.0f;


        bool    s_xActive = false;
        Vector3 s_xSmoothed;
        float   s_xHold = 0.0f;
        float   s_xBlockDist = 0.0f;

        constexpr uintptr_t kReticleScreenPosRva = 0xC72670;

        constexpr float kReticleBlendInTime     = 0.12f;
        constexpr float kReticleBlendOutTime    = 0.10f;

        std::atomic<uint64_t> s_reticleOverride{ 0 };
        std::atomic<uint32_t> s_reticleStrength{ 0 };
        std::atomic<uint32_t> s_displaceAllowed{ 1 };
        SafetyHookInline s_hkReticlePos;

        float   s_reticleBlendT = 0.0f;
        Vector2 s_reticleTarget{};

        constexpr uintptr_t kReticleUpdateBounds[][2] = {
            { 0xF71B80, 0xF71D34 },
            { 0xF72E90, 0xF73227 },
            { 0xF744C0, 0xF7460D },
            { 0xF75180, 0xF75561 },
            { 0xF75880, 0xF7590D },
            { 0xF75CF0, 0xF75DBD },
            { 0xF762C0, 0xF76475 },
            { 0xF7B310, 0xF7B39D },
        };

        bool RetAddrIsReticleUpdate(uintptr_t ret)
        {
            const uintptr_t rva = ret - g_base;
            for (const auto& b : kReticleUpdateBounds) {
                if (rva >= b[0] && rva < b[1])
                    return true;
            }
            return false;
        }

        __declspec(noinline) float* __fastcall hk_ReticleScreenPos(float* ctrl, float* out)
        {
            const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            float* r = s_hkReticlePos.fastcall<float*>(ctrl, out);
            if (!RetAddrIsReticleUpdate(ret))
                return r;
            const uint64_t packed = s_reticleOverride.load(std::memory_order_relaxed);
            const uint32_t sbits  = s_reticleStrength.load(std::memory_order_relaxed);
            if (packed && sbits && r &&
                s_displaceAllowed.load(std::memory_order_relaxed)) {
                const float st = std::bit_cast<float>(sbits);
                const float tx = std::bit_cast<float>(static_cast<uint32_t>(packed));
                const float ty = std::bit_cast<float>(static_cast<uint32_t>(packed >> 32));
                const float dx = (tx - r[0]) * st;
                const float dy = (ty - r[1]) * st;
                if (std::isfinite(dx) && std::isfinite(dy)) {
                    r[0] += dx;
                    r[1] += dy;
                }
            }
            return r;
        }

        void EnsureReticleHook()
        {
            static bool attempted = false;
            if (attempted) return;
            attempted = true;
            void* target = reinterpret_cast<void*>(
                reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) +
                kReticleScreenPosRva);
            s_hkReticlePos = safetyhook::create_inline(
                target, reinterpret_cast<void*>(hk_ReticleScreenPos));
            TO1_LOG("[BARREL] reticle screen-pos hook (rva 0x%llx): %s\n",
                static_cast<unsigned long long>(kReticleScreenPosRva),
                s_hkReticlePos ? "installed" : "FAILED");
        }

        void ClearReticleOverride()
        {
            s_reticleBlendT = 0.0f;
            s_reticleStrength.store(0, std::memory_order_relaxed);
            s_reticleOverride.store(0, std::memory_order_relaxed);
        }

        void UpdateReticleBlend(float dt, bool wantOverride, const Vector2& target)
        {
            if (wantOverride)
                s_reticleTarget = target;

            if (wantOverride)
                s_reticleBlendT += dt / kReticleBlendInTime;
            else
                s_reticleBlendT -= dt / kReticleBlendOutTime;
            if (s_reticleBlendT < 0.0f) s_reticleBlendT = 0.0f;
            if (s_reticleBlendT > 1.0f) s_reticleBlendT = 1.0f;

            if (s_reticleBlendT <= 0.0f) {
                s_reticleStrength.store(0, std::memory_order_relaxed);
                s_reticleOverride.store(0, std::memory_order_relaxed);
                return;
            }

            const float t = s_reticleBlendT;
            const float eased = t * t * (3.0f - 2.0f * t);
            const uint64_t packed =
                static_cast<uint64_t>(std::bit_cast<uint32_t>(s_reticleTarget.x)) |
                (static_cast<uint64_t>(std::bit_cast<uint32_t>(s_reticleTarget.y)) << 32);
            s_reticleOverride.store(packed, std::memory_order_relaxed);
            s_reticleStrength.store(std::bit_cast<uint32_t>(eased),
                std::memory_order_relaxed);
        }

        constexpr uintptr_t kReticleStateUpdateRvas[] = {
            0xF71B80, 0xF72E90, 0xF744C0, 0xF75180,
            0xF75880, 0xF75CF0, 0xF762C0, 0xF7B310 };
        constexpr uintptr_t kSetWidgetColorRva = 0x1E5B7C0;
        constexpr int kReticleStateHookCount =
            int(sizeof(kReticleStateUpdateRvas) / sizeof(kReticleStateUpdateRvas[0]));

        constexpr float kReticleGrey[4]  = { 0.70f, 0.70f, 0.70f, 0.80f };
        constexpr float kReticleWhite[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

        SafetyHookInline s_hkReticleState[kReticleStateHookCount];
        std::atomic<uintptr_t> s_reticleNode{ 0 };
        std::atomic<uint64_t>  s_reticleNodeStamp{ 0 };
        std::atomic<uint64_t>  s_tickStamp{ 0 };
        uintptr_t s_greyedNode = 0;
        float     s_greyedOrig[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

        constexpr uintptr_t kGui2NodeColorOffset = 928;
        constexpr uintptr_t kGui2NodeAlphaOffset = 956;

        using SetColorFn = void(__fastcall*)(uintptr_t, const float*);

        void RecomputeReticleCompositeOrigin(uintptr_t node);

        template <int N>
        uint64_t __fastcall hk_ReticleStateUpdate(uintptr_t node, uint64_t a2, uint64_t a3, uint64_t a4)
        {
            s_reticleNode.store(node, std::memory_order_release);
            s_reticleNodeStamp.store(s_tickStamp.load(std::memory_order_relaxed),
                std::memory_order_release);
            if (node) {
                const float aIn =
                    *reinterpret_cast<const float*>(node + kGui2NodeAlphaOffset);
                s_displaceAllowed.store(aIn > 0.01f ? 1u : 0u,
                    std::memory_order_relaxed);
            }
            const uint64_t r = s_hkReticleState[N].fastcall<uint64_t>(node, a2, a3, a4);
            s_displaceAllowed.store(1, std::memory_order_relaxed);
            if (node &&
                net::gui2::GetReticleMode() == net::gui2::ReticleMode::MovingReticle)
                RecomputeReticleCompositeOrigin(node);
            return r;
        }

        void EnsureReticleNodeHooks()
        {
            static bool attempted = false;
            if (attempted) return;
            attempted = true;
            using StateFn = uint64_t(__fastcall*)(uintptr_t, uint64_t, uint64_t, uint64_t);
            constexpr StateFn thunks[kReticleStateHookCount] = {
                hk_ReticleStateUpdate<0>, hk_ReticleStateUpdate<1>,
                hk_ReticleStateUpdate<2>, hk_ReticleStateUpdate<3>,
                hk_ReticleStateUpdate<4>, hk_ReticleStateUpdate<5>,
                hk_ReticleStateUpdate<6>, hk_ReticleStateUpdate<7> };
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
            int ok = 0;
            for (int i = 0; i < kReticleStateHookCount; ++i) {
                s_hkReticleState[i] = safetyhook::create_inline(
                    reinterpret_cast<void*>(base + kReticleStateUpdateRvas[i]),
                    reinterpret_cast<void*>(thunks[i]));
                if (s_hkReticleState[i]) ++ok;
            }
            TO1_LOG("[BARREL] reticle state hooks: %d/%d installed\n",
                ok, kReticleStateHookCount);
        }

        constexpr uintptr_t kGetSystemRva      = 0x1270A80;

        constexpr uintptr_t kCompositeOriginRecomputeRva     = 0x1E4EE00;
        constexpr uintptr_t kGui2NodeFlags557Offset          = 557;
        constexpr uint8_t   kGui2NodeCompositedBit           = 0x40;
        constexpr uintptr_t kGui2NodeCompositeSlotOffset     = 708;
        constexpr uintptr_t kGui2MgrCompositeGridDimOffset   = 1973324;
        constexpr int       kGui2SystemId                    = 23;

        void RecomputeReticleCompositeOrigin(uintptr_t node)
        {
            const uint8_t flags =
                *reinterpret_cast<const uint8_t*>(node + kGui2NodeFlags557Offset);
            if ((flags & kGui2NodeCompositedBit) == 0)
                return;
            auto getSystem =
                GameFn<uintptr_t(__fastcall*)(int)>(static_cast<uint32_t>(kGetSystemRva));
            const uintptr_t sys = getSystem(kGui2SystemId);
            if (!sys)
                return;
            const uint8_t dim =
                *reinterpret_cast<const uint8_t*>(sys + kGui2MgrCompositeGridDimOffset);
            const uint8_t slot =
                *reinterpret_cast<const uint8_t*>(node + kGui2NodeCompositeSlotOffset);
            if (slot >= static_cast<uint8_t>(dim * dim))
                return;
            auto recompute = GameFn<void(__fastcall*)(uintptr_t, uint64_t, uint64_t)>(
                static_cast<uint32_t>(kCompositeOriginRecomputeRva));
            recompute(node, 0, 0);
        }


        void CaptureNodeColor(uintptr_t node, float out[4])
        {
            const uint32_t packed =
                *reinterpret_cast<const uint32_t*>(node + kGui2NodeColorOffset);
            out[0] = static_cast<float>(packed & 0xFF) / 255.0f;
            out[1] = static_cast<float>((packed >> 8) & 0xFF) / 255.0f;
            out[2] = static_cast<float>((packed >> 16) & 0xFF) / 255.0f;
            out[3] = static_cast<float>((packed >> 24) & 0xFF) / 255.0f;

            const bool looksGrey =
                std::fabs(out[0] - kReticleGrey[0]) < 0.02f &&
                std::fabs(out[1] - kReticleGrey[1]) < 0.02f &&
                std::fabs(out[2] - kReticleGrey[2]) < 0.02f;
            if (packed == 0 || looksGrey) {
                out[0] = kReticleWhite[0];
                out[1] = kReticleWhite[1];
                out[2] = kReticleWhite[2];
                out[3] = kReticleWhite[3];
            }
        }

        bool CurrentReticleNode(uint64_t now, uintptr_t& outNode)
        {
            for (int attempt = 0; attempt < 4; ++attempt) {
                const uint64_t  stamp = s_reticleNodeStamp.load(std::memory_order_acquire);
                const uintptr_t node  = s_reticleNode.load(std::memory_order_acquire);
                if (stamp != s_reticleNodeStamp.load(std::memory_order_acquire))
                    continue;
                outNode = node;
                return node != 0 && now - stamp <= 2;
            }
            outNode = 0;
            return false;
        }

        void RestoreGreyedNode(SetColorFn setColor, bool live)
        {
            if (!s_greyedNode) return;
            if (live)
                setColor(s_greyedNode, s_greyedOrig);
            s_greyedNode = 0;
        }

        void UpdateReticleGrey(bool wantGrey)
        {
            const uint64_t now = s_tickStamp.fetch_add(1, std::memory_order_relaxed) + 1;
            if (wantGrey)
                EnsureReticleNodeHooks();
            uintptr_t node = 0;
            const bool fresh = CurrentReticleNode(now, node);
            auto setColor = GameFn<SetColorFn>(static_cast<uint32_t>(kSetWidgetColorRva));

            if (s_greyedNode && (!wantGrey || s_greyedNode != node))
                RestoreGreyedNode(setColor, fresh && s_greyedNode == node);

            if (!wantGrey || !fresh)
                return;

            if (s_greyedNode != node) {
                CaptureNodeColor(node, s_greyedOrig);
                s_greyedNode = node;
            }
            setColor(node, kReticleGrey);
        }

        bool ReadLocatorPos(uint64_t locator, Vector3& out)
        {
            if (!locator) return false;
            const Vector3* p = DC::Call<"get-locator-position", const Vector3>(locator);
            if (!p) return false;
            out = *p;
            return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
        }

        float Dist(const Vector3& a, const Vector3& b)
        {
            const Vector3 d = a - b;
            return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        }

        bool WorldToScreen(const Vector3& p, Vector2& out)
        {
            const Vector2* f = DC::Call<"world-space-to-screen-space", const Vector2>(0, MEMORY::ALLOC_VECTOR(p));
            if (!f) return false;
            out = *f;
            return std::isfinite(out.x) && std::isfinite(out.y) &&
                   out.x >= 0.0f && out.x <= 1280.0f &&
                   out.y >= 0.0f && out.y <= 720.0f;
        }

        void DrawMarker2D(const Vector2& c, float half, uint32_t color)
        {
            DebugDrawCommon::DrawRectangle(c.x - half, c.y - half,
                                           c.x + half, c.y + half, color);
        }

        Quaternion QuatFromZTo(const Vector3& d)
        {
            const float w = 1.0f + d.z;
            if (w < 1e-4f)
                return { 1.0f, 0.0f, 0.0f, 0.0f };
            Quaternion q(-d.y, d.x, 0.0f, w);
            const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            return { q.x / len, q.y / len, q.z / len, q.w / len };
        }

        void SpawnImpactFx(const Vector3& pos, const Vector3& dir, int count)
        {
            const Quaternion rot = QuatFromZTo(dir);
            for (int i = 0; i < count; i++) {
                const uint64_t frame = MEMORY::ALLOC_BOUND_FRAME(pos, rot);
                if (!frame) return;
                const uint64_t handle = DC::Call<"spawn-particles-at-bound-frame">(kImpactSquib, frame, 0);
                TO1_LOG("[BARREL] impact fx at (%.2f %.2f %.2f) handle=%llx\n",
                    pos.x, pos.y, pos.z, static_cast<unsigned long long>(handle));
            }
        }

        void ResetPending()
        {
            s_camRayId = 0;
            s_barrelRayId = 0;
            s_barrelPending = false;
            s_targetObj = 0;
        }

        bool RayHit(uint64_t rayId)
        {
            const uint64_t raw = DC::Call<"wait-raycast-t">(rayId);
            if (raw == static_cast<uint64_t>(-1)) return false;
            return std::bit_cast<float>(static_cast<uint32_t>(raw)) >= 0.0f;
        }

        uint64_t RightHandLocator(Vector3& outPos)
        {
            Vector3 rootPos{};
            const uint64_t rootLoc = DC::Call<"get-object-locator">(SID("player"));
            if (!ReadLocatorPos(rootLoc, rootPos)) return 0;

            const uint64_t loc = DC::Call<"get-joint-locator">(SID("player"),
                SID("r_hand_prop_attachment"));
            if (!ReadLocatorPos(loc, outPos)) return 0;

            if (g_rHandVOffset > 0.0f) {
                Vector3 lifted = outPos;
                Vector3 side{};
                const uint64_t sv  = MEMORY::ALLOC_VECTOR({ g_rHandHOffset, 0.0f, 0.0f });
                const uint64_t off = sv ? DC::Call<"offset-locator">(rootLoc, sv) : 0;
                if (off && ReadLocatorPos(off, side)) {
                    lifted.x += side.x - rootPos.x;
                    lifted.z += side.z - rootPos.z;
                }
                lifted.y += g_rHandVOffset;
                lifted.x += g_rHandHOffset;
                const uint64_t pt = DC::Call<"alloc-point">(lifted.x, lifted.y,
                    lifted.z, 0);
                if (pt && pt != static_cast<uint64_t>(-1)) {
                    DC::Call<"set-locator-position">(loc, pt);
                    Vector3 check{};
                    if (ReadLocatorPos(loc, check))
                        outPos = check;
                }
            }

            return loc;
        }
    }

    bool ShotBlocked()
    {
        return g_enabled && s_blocked.load(std::memory_order_relaxed);
    }

    namespace {
        struct LatchedProjectile {
            uint32_t id = 0;
            bool     blocked = false;
        };
        constexpr uintptr_t kProjectileBulletArgs = 0x6C0;
        constexpr uintptr_t kProcessIdOffset = 148;
        std::mutex        s_latchLock;
        LatchedProjectile s_latched[8];
        unsigned          s_latchNext = 0;
    }

    void LatchProjectile(uintptr_t process)
    {
        if (!process) return;
        const uint32_t id = *reinterpret_cast<const uint32_t*>(process + kProcessIdOffset);
        if (!id) return;
        const bool blocked = ShotBlocked();
        std::lock_guard<std::mutex> g(s_latchLock);
        s_latched[s_latchNext] = { id, blocked };
        s_latchNext = (s_latchNext + 1) % std::size(s_latched);
    }

    bool ImpactBlocked(uintptr_t bulletArgs)
    {
        if (bulletArgs >= kProjectileBulletArgs) {
            const uint32_t id = *reinterpret_cast<const uint32_t*>(
                bulletArgs - kProjectileBulletArgs + kProcessIdOffset);
            std::lock_guard<std::mutex> g(s_latchLock);
            for (const LatchedProjectile& e : s_latched)
                if (id && e.id == id)
                    return e.blocked;
        }
        return ShotBlocked();
    }

    void NotifyDeniedImpact()
    {
        s_pendingFx.fetch_add(1, std::memory_order_relaxed);
    }

    void Tick()
    {
        if (!g_enabled || !ENTITY::DOES_EXIST(SID("player"))) {
            ResetPending();
            s_pendingFx.store(0, std::memory_order_relaxed);
            s_lastBlockValid = false;
            s_xActive = false;
            ClearReticleOverride();
            UpdateReticleGrey(false);
            s_blocked.store(false, std::memory_order_relaxed);
            return;
        }

        if (const int fx = s_pendingFx.exchange(0, std::memory_order_relaxed);
            fx > 0 && s_lastBlockValid)
            SpawnImpactFx(s_lastBlockPos, s_lastBlockDir,
                fx < kMaxFxPerTick ? fx : kMaxFxPerTick);

        if (!DC::Call<"player-is-aiming?">()) {
            ResetPending();
            s_xActive = false;
            ClearReticleOverride();
            UpdateReticleGrey(false);
            s_blocked.store(false, std::memory_order_relaxed);
            return;
        }

        uint64_t camHitLoc = 0;
        uint64_t camHitObj = 0;
        Vector3  camHit{};
        bool     camHitValid = false;
        if (s_camRayId) {
            const bool camMissed = !RayHit(s_camRayId);
            camHitLoc = DC::Call<"wait-raycast">(s_camRayId);
            if (!camMissed && camHitLoc) {
                camHitValid = ReadLocatorPos(camHitLoc, camHit);
                camHitObj = DC::Call<"wait-raycast-get-object">(s_camRayId);
            }
        }

        bool    blocked = false;
        Vector3 blockPos{};
        float   blockMiss = 0.0f;
        float   blockDist = 0.0f;
        if (s_barrelRayId && s_barrelPending) {
            const bool barrelMissed = !RayHit(s_barrelRayId);
            if (!barrelMissed) {
                if (const uint64_t loc = DC::Call<"wait-raycast">(s_barrelRayId)) {
                    Vector3 q;
                    if (ReadLocatorPos(loc, q)) {
                        const uint64_t hitObj =
                            DC::Call<"wait-raycast-get-object">(s_barrelRayId);
                        blockMiss = Dist(q, s_barrelTarget);
                        const bool sameObject = hitObj && hitObj == s_targetObj;
                        if (!sameObject && blockMiss > kAlignTolerance) {
                            blocked = true;
                            blockPos = q;
                            const Vector3 back = s_barrelFrom - q;
                            blockDist= Dist(s_barrelFrom, q);
                            if (blockDist > 1e-3f) {
                                s_lastBlockPos = q;
                                s_lastBlockDir = back / blockDist;
                                s_lastBlockValid = true;
                            }
                        }
                    }
                }
            }
        }

        const bool wasBlocked = s_blocked.exchange(blocked, std::memory_order_relaxed);
        if (wasBlocked != blocked) {
            if (blocked) {
                TO1_LOG("[BARREL] BLOCKED  ray origin=r-hand (%.2f %.2f %.2f) stopped at "
                        "(%.2f %.2f %.2f), %.2fm short of camera hit (%.2f %.2f %.2f)\n",
                    s_barrelFrom.x, s_barrelFrom.y, s_barrelFrom.z,
                    blockPos.x, blockPos.y, blockPos.z, blockMiss,
                    s_barrelTarget.x, s_barrelTarget.y, s_barrelTarget.z);
            }
            else {
                TO1_LOG("[BARREL] clear\n");
            }
        }

        const float dt = TIME::GET_DELTA_TIME();
        if (blocked) {
            if (!s_xActive || Dist(blockPos, s_xSmoothed) > kXSnapDist) {
                s_xSmoothed = blockPos;
            } else {
                const float a = 1.0f - std::exp(-dt / kXSmoothTau);
                s_xSmoothed = s_xSmoothed + (blockPos - s_xSmoothed) * a;
            }
            s_xActive = true;
            s_xHold = kXHoldTime;
            s_xBlockDist = blockDist;
        } else if (s_xActive) {
            s_xHold -= dt;
            if (s_xHold <= 0.0f)
                s_xActive = false;
        }

        bool    wantOverride = false;
        Vector2 overrideS{};
        if (s_xActive) {
            Vector2 s;
            if (WorldToScreen(s_xSmoothed, s) &&
                s_xBlockDist <= kMinBlockDistanceDrawMarker) {
                const net::gui2::ReticleMode mode = net::gui2::GetReticleMode();
                if (mode == net::gui2::ReticleMode::MovingReticle) {
                    EnsureReticleHook();
                    EnsureReticleNodeHooks();
                    if (s_hkReticlePos) {
                        wantOverride = true;
                        overrideS = s;
                    }
                }
#ifdef TO1_DEBUG
                if (g_drawDebug) {
                    constexpr float kXScale = 1.2f;
                    const vec2 sz = DebugDrawCommon::GetTextWidthHeight("X", kXScale);
                    DebugDrawCommon::DrawTextPrintV(s.x - sz.x * 0.5f, s.y - sz.y * 0.5f,
                        kXScale, kColBlocked, "X - %.2f", s_xBlockDist);
                }
                else
#endif
                if (mode == net::gui2::ReticleMode::MarkerWidget) {
                    constexpr float kXScale = 1.2f;
                    const vec2 sz = DebugDrawCommon::GetTextWidthHeight("X", kXScale);
                    DebugDrawCommon::DrawTextPrintV(s.x - sz.x * 0.5f, s.y - sz.y * 0.5f,
                        kXScale, kColBlocked, "X");
                }
            }
        }
        UpdateReticleBlend(dt, wantOverride, overrideS);
        UpdateReticleGrey(s_xActive &&
            net::gui2::GetReticleMode() == net::gui2::ReticleMode::GreyReticle);

#ifdef TO1_DEBUG
        if (g_drawDebug) {
            Vector2 s;
            if (camHitValid && WorldToScreen(camHit, s))
                DrawMarker2D(s, 3.0f, kColTarget);
            if (camHitValid)
                engine::debugdraw::Line(s_barrelFrom, s_barrelTarget,
                    blocked ? kColBlocked : kColClear);
            const char* status = blocked ? "<color=red>BARREL BLOCKED</color>"
                                         : "<color=green>BARREL CLEAR</color>";
            DebugDrawCommon::DrawTextPrintV(600.0f, 420.0f, 0.8f,
                blocked ? kColBlocked : kColClear, "%s", status);
            if (blocked)
                DebugDrawCommon::DrawTextPrintV(600.0f, 440.0f, 0.7f, kColBlocked,
                    "miss %.2fm", blockMiss);
            DebugDrawCommon::DrawTextPrintV(600.0f, 460.0f, 0.7f, kColTarget,
                "origin r-hand (%.2f %.2f %.2f)",
                s_barrelFrom.x, s_barrelFrom.y, s_barrelFrom.z);
            if (const uint64_t surfSid = engine::meshray::LastSurfaceSid()) {
                const std::string surf =
                    NdGameSdk::Sidbase::StringIdToStringInternal(surfSid);
                DebugDrawCommon::DrawTextPrintV(600.0f, 480.0f, 0.7f, kColTarget,
                    "surface %s", surf.c_str());
            }
            else if (engine::meshray::LastHitValid()) {
                DebugDrawCommon::DrawTextPrintV(600.0f, 480.0f, 0.7f, kColTarget,
                    "surface <passthrough>");
            }
        }
#endif

        s_camRayId = 0;
        const uint64_t camFrom = DC::Call<"camera-through-reticle-locator">();
        if (camFrom) {
            const uint64_t fwd = MEMORY::ALLOC_VECTOR({ 0.0f, 0.0f, kCamRayLength });
            const uint64_t camTo = fwd ? DC::Call<"offset-locator">(camFrom, fwd) : 0;
            if (camTo)
                s_camRayId = DC::Call<"kick-raycast">(camFrom, camTo,
                    SID("player"), kRaycastMask);
#ifdef TO1_DEBUG
            if (g_drawDebug && camHitValid) {
                Vector3 camPos{};
                if (ReadLocatorPos(camFrom, camPos)) {
                    const Vector3 d = camHit - camPos;
                    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                    if (len > 0.1f)
                        engine::meshray::Kick(camPos, camHit + d / len * 0.5f);
                }
            }
#endif
        }

        s_barrelRayId = 0;
        s_barrelPending = false;
        s_targetObj = 0;
        if (camHitValid && camHitLoc) {
            Vector3 castFromPos{};
            const uint64_t castFrom = RightHandLocator(castFromPos);
            if (castFrom) {
                s_barrelRayId = DC::Call<"kick-raycast">(castFrom, camHitLoc,
                    SID("player"), kRaycastMask);
                if (s_barrelRayId) {
                    s_barrelPending = true;
                    s_barrelFrom = castFromPos;
                    s_barrelTarget = camHit;
                    s_targetObj = camHitObj;
                }
            }
        }
    }
}
