// ported from tools/ext/t2r-ext.c (g_clickEdge / ui_click_gate_tick, MenuEdge and
// UiSelectIsMouse). One sampler, no keyboard hook.

#include "menu-input.hpp"

#include "window.hpp"
#include "script.hpp"

#include <windows.h>

namespace engine::menu_input {

    namespace {
        struct State {
            bool     lbDown   = false;
            bool     click    = false;
            uint64_t now      = 0;
            uint64_t lbUpAt   = 0;
        } g;

        constexpr uint64_t kMouseSelectMs = 350;
    }

    void BeginFrame(uint64_t now)
    {
        const bool down = engine::window::KeyDown(VK_LBUTTON);
        g.now = now;
        g.click = down && !g.lbDown;
        if (!down && g.lbDown)
            g.lbUpAt = now;
        g.lbDown = down;
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

    bool CommandEdge(StringId64 command, bool& held)
    {
        const bool now = DC::JOYPAD_COMMAND_ACTIVE(command);
        const bool edge = now && !held;
        held = now;
        return edge;
    }

    void Reset()
    {
        g = State{};
    }

} // namespace engine::menu_input
