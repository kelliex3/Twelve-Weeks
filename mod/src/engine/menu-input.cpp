// ported from tools/ext/t2r-ext.c (g_clickEdge / ui_click_gate_tick, MenuEdge,
// UiSelectIsMouse, wheel_notches, pad_state and ui_input_mode_tick). One shared
// per-frame sampler; no system-wide keyboard / mouse hook is installed.

#include "menu-input.hpp"

#include "window.hpp"
#include "log.hpp"             // TO1_LOG must precede script.hpp
#include "script.hpp"

#include <windows.h>

namespace engine::menu_input {

    namespace {

        // XINPUT_STATE's layout, declared locally so this module needs no xinput
        // import library (the ext loaded xinput1_4.dll dynamically too).
        struct XState {
            uint32_t packet;
            uint16_t buttons;
            uint8_t  lt, rt;
            int16_t  lx, ly, rx, ry;
        };
        using XGetState = uint32_t (WINAPI *)(uint32_t, void*);

        struct PadFrame {
            uint16_t buttons = 0;
            uint8_t  lt = 0, rt = 0;
            int16_t  lx = 0, ly = 0, rx = 0, ry = 0;
        };

        // The wheel hook is installed on the foreground game window's thread only
        // while the game is focused. WM_INPUT is also watched for games that take
        // raw input instead of WM_MOUSEWHEEL; the first seen path wins, so a notch
        // is never counted twice.
        struct Wheel {
            HHOOK        hook = nullptr;
            DWORD        tid = 0;
            int          tries = 0;
            LONG         rem = 0;
            volatile LONG acc = 0, raw = 0, legacy = 0;
        } g_wheel;

        LRESULT CALLBACK WheelProc(int code, WPARAM wp, LPARAM lp)
        {
            if (code == HC_ACTION && wp == PM_REMOVE) {
                const MSG* m = reinterpret_cast<const MSG*>(lp);
                if (m->message == WM_MOUSEWHEEL) {
                    InterlockedAdd(&g_wheel.acc,
                        static_cast<LONG>(GET_WHEEL_DELTA_WPARAM(m->wParam)));
                    InterlockedExchange(&g_wheel.legacy, 1);
                }
                else if (m->message == WM_INPUT) {
                    RAWINPUT ri{};
                    UINT sz = sizeof ri;
                    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(m->lParam),
                            RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)
                        && ri.header.dwType == RIM_TYPEMOUSE
                        && (ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL)) {
                        InterlockedAdd(&g_wheel.raw,
                            static_cast<LONG>(static_cast<short>(ri.data.mouse.usButtonData)));
                    }
                }
            }
            return CallNextHookEx(nullptr, code, wp, lp);
        }

        void InstallWheel()
        {
            HWND fg = GetForegroundWindow();
            DWORD pid = 0;
            const DWORD tid = fg ? GetWindowThreadProcessId(fg, &pid) : 0;
            if (!tid || pid != GetCurrentProcessId())
                return;
            if (g_wheel.hook && g_wheel.tid != tid) {
                UnhookWindowsHookEx(g_wheel.hook);
                g_wheel.hook = nullptr;
            }
            if (g_wheel.hook || g_wheel.tries > 3)
                return;
            ++g_wheel.tries;
            g_wheel.tid = tid;
            g_wheel.hook = SetWindowsHookExW(WH_GETMESSAGE, WheelProc, nullptr, tid);
        }

        int TakeWheel(uint64_t now)
        {
            InstallWheel();
            const LONG legacy = InterlockedExchange(&g_wheel.acc, 0);
            const LONG raw = InterlockedExchange(&g_wheel.raw, 0);
            const int delta = static_cast<int>(g_wheel.legacy ? legacy : raw);
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            if (pid != GetCurrentProcessId())
                return 0;
            (void)now;
            g_wheel.rem += delta;
            const int n = g_wheel.rem / WHEEL_DELTA;
            g_wheel.rem -= n * WHEEL_DELTA;
            return n;
        }

        bool SamplePad(uint64_t now, PadFrame& out)
        {
            static XGetState xs = nullptr;
            static bool tried = false;
            static int slot = -1, have = 0;
            static uint64_t frame = ~uint64_t{ 0 }, nextProbe = 0;
            static XState st{};

            if (!tried) {
                tried = true;
                if (HMODULE m = LoadLibraryW(L"xinput1_4.dll"))
                    xs = reinterpret_cast<XGetState>(GetProcAddress(m, "XInputGetState"));
            }
            if (!xs || !engine::window::Focused())
                return false;
            if (frame != now) {
                frame = now;
                have = 0;
                if (slot >= 0 && xs(static_cast<uint32_t>(slot), &st) == 0)
                    have = 1;
                else if (now >= nextProbe) {
                    slot = -1;
                    nextProbe = now + 2000;         // empty slots are slow: probe at 2 Hz
                    for (uint32_t u = 0; u < 4 && !have; ++u)
                        if (xs(u, &st) == 0) { slot = static_cast<int>(u); have = 1; }
                }
                if (!have && slot >= 0)
                    slot = -1;
            }
            if (!have)
                return false;
            out.buttons = st.buttons;
            out.lt = st.lt;
            out.rt = st.rt;
            out.lx = st.lx;
            out.ly = st.ly;
            out.rx = st.rx;
            out.ry = st.ry;
            return true;
        }

        struct State {
            bool     lbDown   = false;
            bool     click    = false;
            uint64_t now      = 0;
            uint64_t lbUpAt   = 0;
            int      wheel    = 0;
            bool     padOk    = false;
            PadFrame pad{};
            bool     padMode  = false;
            POINT    lastCursor{};
            bool     haveCursor = false;
        } g;

        constexpr uint64_t kMouseSelectMs = 350;
        constexpr int      kPadDead = 16000;
    }

    void BeginFrame(uint64_t now)
    {
        const bool down = engine::window::KeyDown(VK_LBUTTON);
        g.now = now;
        g.click = down && !g.lbDown;
        if (!down && g.lbDown)
            g.lbUpAt = now;
        g.lbDown = down;

        g.wheel = TakeWheel(now);
        g.padOk = SamplePad(now, g.pad);

        // ext ui_input_mode_tick: the last device used wins. Keyboard / mouse input
        // clears pad mode; any real pad button, trigger or stick past the dead zone
        // sets it. The pages use this to pick key labels vs button glyphs.
        static const int keys[] = { VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN,
                                    'A', 'D', 'W', 'S', VK_SPACE, VK_RETURN, VK_TAB };
        bool mnk = g.click || g.wheel != 0;
        if (engine::window::Focused())
            for (int k : keys) mnk = mnk || engine::window::KeyDown(k);
        POINT pt{};
        if (GetCursorPos(&pt)) {
            if (g.haveCursor && (pt.x != g.lastCursor.x || pt.y != g.lastCursor.y)
                && engine::window::Focused())
                mnk = true;
            g.lastCursor = pt;
            g.haveCursor = true;
        }
        if (mnk)
            g.padMode = false;
        if (g.padOk && (g.pad.buttons || g.pad.lt > 80 || g.pad.rt > 80
                        || g.pad.lx < -kPadDead || g.pad.lx > kPadDead
                        || g.pad.ly < -kPadDead || g.pad.ly > kPadDead
                        || g.pad.rx < -kPadDead || g.pad.rx > kPadDead
                        || g.pad.ry < -kPadDead || g.pad.ry > kPadDead))
            g.padMode = true;
    }

    bool ClickEdge() { return g.click; }

    bool SelectIsMouse()
    {
        if (engine::window::KeyDown(VK_SPACE) || engine::window::KeyDown(VK_RETURN))
            return false;
        if (engine::window::KeyDown(VK_LBUTTON))
            return true;
        return g.lbUpAt && g.now - g.lbUpAt < kMouseSelectMs;
    }

    bool CommandActive(StringId64 command)
    {
        return DC::JOYPAD_COMMAND_ACTIVE(command);
    }

    bool CommandEdge(StringId64 command, bool& held)
    {
        const bool now = CommandActive(command);
        const bool edge = now && !held;
        held = now;
        return edge;
    }

    bool CommandRepeat(StringId64 command, bool& held, uint64_t& next, uint64_t now)
    {
        // gearup13.c: GU_REP_FIRST_MS 260, GU_REP_MS 75.
        constexpr uint64_t kFirst = 260;
        constexpr uint64_t kRepeat = 75;
        const bool on = CommandActive(command);
        bool fire = false;
        if (on && !held) {
            fire = true;
            next = now + kFirst;
        }
        else if (on && held && now >= next) {
            fire = true;
            next = now + kRepeat;
        }
        held = on;
        return fire;
    }

    int WheelNotches() { return g.wheel; }

    uint16_t PadButtons() { return g.padOk ? g.pad.buttons : 0; }

    bool RightStick(float& x, float& y)
    {
        if (!g.padOk)
            return false;
        x = static_cast<float>(g.pad.rx) / 32768.0f;
        y = static_cast<float>(g.pad.ry) / 32768.0f;
        return true;
    }

    bool LeftStick(float& x, float& y)
    {
        if (!g.padOk)
            return false;
        x = static_cast<float>(g.pad.lx) / 32768.0f;
        y = static_cast<float>(g.pad.ly) / 32768.0f;
        return true;
    }

    bool Triggers(float& lt, float& rt)
    {
        if (!g.padOk)
            return false;
        lt = static_cast<float>(g.pad.lt) / 255.0f;
        rt = static_cast<float>(g.pad.rt) / 255.0f;
        return true;
    }

    bool PadMode() { return g.padMode; }

    void Reset()
    {
        g = State{};
        g_wheel.rem = 0;
        InterlockedExchange(&g_wheel.acc, 0);
        InterlockedExchange(&g_wheel.raw, 0);
    }

} // namespace engine::menu_input
