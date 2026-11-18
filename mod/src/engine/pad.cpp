// ported from tools/ext/padinject.c (PadinjUpdHook's activity stamp, g_padActMs)

#include "pad.hpp"

#include "log.hpp"

#include <windows.h>
#include <safetyhook.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace engine::pad {

    namespace {

        // exe RVA of Joypad::Update(Joypad*, Pad*, int, int) (ext PADINJ_UPD_RVA).
        constexpr uint32_t kJoypadUpdateRva = 0x1593350;

        constexpr size_t kBtnOffset = 0x528;   // u64 button mask in the Pad
        constexpr float  kStick     = 0.3f;    // ext AFK_STICK / padinj's own rest threshold

        // ext padinject.c prologue guard: push rbx; push r13; sub rsp, 0x168.
        constexpr uint8_t kPrologue[11] = {
            0x40, 0x53, 0x41, 0x55, 0x48, 0x81, 0xEC, 0x68, 0x01, 0x00, 0x00 };

        // One last-button-mask per Pad object. The engine reads several Pads through
        // Joypad::Update; a single global mask would flap when they alternate.
        constexpr int kMaxPads = 8;
        struct Track {
            const void* pad = nullptr;
            uint64_t    btn = 0;
        };
        Track g_tracks[kMaxPads];

        std::atomic<uint64_t> g_actMs{ 0 };
        SafetyHookInline      g_hook;
        int                   g_state = 0;   // 0 untried, 1 installed, -1 failed

        bool SawInput(const void* pad)
        {
            if (!pad)
                return false;
            const uint64_t bm = *reinterpret_cast<const uint64_t*>(
                static_cast<const uint8_t*>(pad) + kBtnOffset);
            const float* f = static_cast<const float*>(pad);
            const bool moved =
                std::fabs(f[0]) > kStick || std::fabs(f[1]) > kStick ||
                std::fabs(f[2]) > kStick || std::fabs(f[3]) > kStick;

            Track* free = nullptr;
            for (Track& t : g_tracks) {
                if (t.pad == pad) {
                    const bool edge = bm != t.btn;
                    t.btn = bm;
                    return edge || moved;
                }
                if (!t.pad && !free)
                    free = &t;
            }
            // First sight of this Pad: remember it (a held button is not an edge here).
            (free ? free : &g_tracks[0])->pad = pad;
            (free ? free : &g_tracks[0])->btn = bm;
            return moved;
        }

        uint64_t __fastcall HookJoypadUpdate(void* jp, uint8_t* pad, uint64_t r8, uint64_t r9)
        {
            if (SawInput(pad))
                g_actMs.store(GetTickCount64(), std::memory_order_relaxed);
            return g_hook.fastcall<uint64_t>(jp, pad, r8, r9);
        }

        void Install()
        {
            if (g_state)
                return;
            g_state = -1;   // assume failure until proven otherwise

            HMODULE exe = GetModuleHandleA(nullptr);
            if (!exe)
                return;
            uint8_t* fn = reinterpret_cast<uint8_t*>(exe) + kJoypadUpdateRva;
            if (std::memcmp(fn, kPrologue, sizeof(kPrologue)) != 0) {
                TO1_LOG("[pad] Joypad::Update prologue differs (exe is not 1.6.10721?) - pad activity off\n");
                return;
            }
            g_hook = safetyhook::create_inline(fn, reinterpret_cast<void*>(HookJoypadUpdate));
            if (!g_hook) {
                TO1_LOG("[pad] Joypad::Update hook failed - pad activity off\n");
                return;
            }
            g_state = 1;
            TO1_LOG("[pad] Joypad::Update activity hook installed (rva 0x%X)\n", kJoypadUpdateRva);
        }

    }

    bool Ready()
    {
        Install();
        return g_state == 1;
    }

    uint64_t ActivityMs()
    {
        return g_actMs.load(std::memory_order_relaxed);
    }

}
