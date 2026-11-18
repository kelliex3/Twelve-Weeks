// ported from tools/ext/t2r-ext.c (GameFocused) and tools/ext/afk.c (GetLastInputInfo)

#include "window.hpp"

#include <windows.h>

namespace engine::window {

    bool Focused()
    {
        // ext GameFocused(): GetWindowThreadProcessId(GetForegroundWindow(), &pid) == our pid.
        // The ext benchmarked this at ~9 ns, so a cache would cost more than the call.
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }

    bool LastInputTick(uint32_t* out)
    {
        if (!out)
            return false;
        LASTINPUTINFO li{};
        li.cbSize = sizeof(li);
        if (!GetLastInputInfo(&li))
            return false;
        *out = li.dwTime;
        return true;
    }

}
