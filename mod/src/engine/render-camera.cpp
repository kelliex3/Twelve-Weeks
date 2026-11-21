// ported from tools/ext/t2r-ext.c (InstallIntroCamHook / t2r_icam_thunk / icam_filter /
// rcam_capture / RcamGet / CamPosDir / SfxSpatial listener)

#include "render-camera.hpp"

#include "engine/flag.hpp"
#include "log.hpp"

#include <safetyhook.hpp>

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace engine::render_camera {

    namespace {

        // Pinned to the ext's exact RVAs (image base 0x140000000).
        constexpr uintptr_t kRvaRcamSet   = 0x1477750;   // FUN_141477750
        constexpr uintptr_t kRvaCallSite  = 0x1B26D10;   // lea rcx,[rdi+130h]; call FUN_141477750
        constexpr uintptr_t kManagerSlot  = 0x92B6C10;   // FUN_141b207b0(0) slot
        constexpr uintptr_t kManagerGen   = 0x92B6C18;
        constexpr uintptr_t kMainCamera   = 0x130;       // main view dst offset in the manager
        constexpr uintptr_t kManagerSize  = 0x1300;

        // The ext writes a 15-byte prologue detour; our observer hooks the byte right
        // after it so it sees RDX = icam_filter's final camera when ext is loaded, or
        // RDX = the engine's blended location when it is not. We never touch f..f+15,
        // so installation order relative to ext does not matter.
        constexpr uintptr_t kObserverRva = kRvaRcamSet + 15;

        std::atomic<uint32_t> g_seq{ 0 };   // even = stable, odd = write in progress
        float                 g_pos[3] = {};
        float                 g_fwd[3] = {};
        uint64_t              g_stampMs = 0;
        safetyhook::MidHook   g_hook;
        bool                  g_installed = false;

        // = ext IcamManager(): local player's camera manager, without the asserts.
        uint8_t* IcamManager(uint8_t* base)
        {
            uint8_t* slot = *reinterpret_cast<uint8_t**>(base + kManagerSlot);
            const int gen = *reinterpret_cast<int*>(base + kManagerGen);
            if (!slot || !gen || IsBadReadPtr(slot, 16))
                return nullptr;
            uint8_t* m = *reinterpret_cast<uint8_t**>(slot + 8);
            if (!m || IsBadReadPtr(m, kManagerSize)
                || *reinterpret_cast<int*>(m + 0x94) != gen)
                return nullptr;
            return m;
        }

        // Render thread. Read-only: never modify dst or the camera block.
        void OnRenderCameraSet(SafetyHookContext& ctx)
        {
            uint8_t* base = reinterpret_cast<uint8_t*>(::GetModuleHandleW(nullptr));
            if (!base)
                return;
            uint8_t* mgr = IcamManager(base);
            if (!mgr || reinterpret_cast<uint8_t*>(ctx.rcx) != mgr + kMainCamera)
                return;                         // main view only (ext rcam_capture)

            const float* r = reinterpret_cast<const float*>(ctx.rdx);
            if (!r || IsBadReadPtr(r, 32))
                return;
            const float q[4] = { r[4], r[5], r[6], r[7] };
            if (!QuaternionOk(q)
                || !std::isfinite(r[0] + r[1] + r[2])
                || std::fabs(r[0]) > 1e6f || std::fabs(r[1]) > 1e6f
                || std::fabs(r[2]) > 1e6f)
                return;

            float fwd[3];
            ForwardFromQuaternion(q, fwd);
            if (!std::isfinite(fwd[0] + fwd[1] + fwd[2]))
                return;

            // Seqlock publish.
            const uint32_t s = g_seq.load(std::memory_order_relaxed);
            g_seq.store(s + 1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            g_pos[0] = r[0]; g_pos[1] = r[1]; g_pos[2] = r[2];
            g_fwd[0] = fwd[0]; g_fwd[1] = fwd[1]; g_fwd[2] = fwd[2];
            g_stampMs = ::GetTickCount64();
            g_seq.store(s + 2, std::memory_order_release);
        }

    } // namespace

    bool Install()
    {
        if (g_installed)
            return true;
        uint8_t* base = reinterpret_cast<uint8_t*>(::GetModuleHandleW(nullptr));
        if (!base)
            return false;

        // Stable build check: the call site is the same bytes ext verifies, and our
        // hook never patches it. (The callee prologue at f may already be ext's detour.)
        static const uint8_t kSite[8] = {
            0x48, 0x8D, 0x8F, 0x30, 0x01, 0x00, 0x00, 0xE8
        };
        if (std::memcmp(base + kRvaCallSite, kSite, sizeof(kSite)) != 0
            || *reinterpret_cast<int32_t*>(base + kRvaCallSite + 8)
                   != static_cast<int32_t>(kRvaRcamSet - (kRvaCallSite + 12))) {
            TO1_LOG("[RCAM] call-site signature mismatch at 0x%llx; render camera not hooked\n",
                static_cast<unsigned long long>(kRvaCallSite));
            return false;
        }

        g_hook = safetyhook::create_mid(
            reinterpret_cast<void*>(base + kObserverRva), OnRenderCameraSet);
        if (!g_hook) {
            TO1_LOG("[RCAM] observer hook failed at 0x%llx\n",
                static_cast<unsigned long long>(kObserverRva));
            return false;
        }
        g_installed = true;
        TO1_LOG("[RCAM] final render camera observer installed at 0x%llx\n",
            static_cast<unsigned long long>(kObserverRva));
        return true;
    }

    bool Get(float pos[3], float fwd[3])
    {
        if (!engine::flag::Absent("t2r-no-render-camera.txt"))
            return false;
        const uint64_t now = ::GetTickCount64();
        for (int tries = 0; tries < 4; ++tries) {
            const uint32_t s0 = g_seq.load(std::memory_order_acquire);
            if (s0 & 1)
                continue;
            const float p[3] = { g_pos[0], g_pos[1], g_pos[2] };
            const float f[3] = { g_fwd[0], g_fwd[1], g_fwd[2] };
            const uint64_t stamp = g_stampMs;
            if (s0 != g_seq.load(std::memory_order_acquire))
                continue;
            if (!Fresh(now, stamp))
                return false;
            pos[0] = p[0]; pos[1] = p[1]; pos[2] = p[2];
            fwd[0] = f[0]; fwd[1] = f[1]; fwd[2] = f[2];
            return true;
        }
        return false;
    }

    void Reset()
    {
        g_seq.store(0, std::memory_order_relaxed);
        g_pos[0] = g_pos[1] = g_pos[2] = 0.0f;
        g_fwd[0] = g_fwd[1] = g_fwd[2] = 0.0f;
        g_stampMs = 0;
    }

} // namespace engine::render_camera
