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

    bool KeyDown(int virtualKey)
    {
        if (!Focused())
            return false;
        return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    }

    bool CursorCanvas(float* x, float* y)
    {
        if (!x || !y)
            return false;
        HWND hw = GetForegroundWindow();
        DWORD pid = 0;
        if (!hw || (GetWindowThreadProcessId(hw, &pid), pid != GetCurrentProcessId()))
            return false;
        POINT pt;
        RECT rc;
        if (!GetCursorPos(&pt) || !ScreenToClient(hw, &pt)
            || !GetClientRect(hw, &rc) || rc.right <= 0 || rc.bottom <= 0)
            return false;
        if (pt.x < 0 || pt.y < 0 || pt.x >= rc.right || pt.y >= rc.bottom)
            return false;
        // The canvas is the centred 16:9 box of the client area (pillar / letterboxing).
        float bw = rc.bottom * (16.0f / 9.0f), bh = static_cast<float>(rc.bottom);
        if (bw > rc.right) {
            bw = static_cast<float>(rc.right);
            bh = rc.right * (9.0f / 16.0f);
        }
        *x = (pt.x - (rc.right - bw) * 0.5f) * 3840.0f / bw;
        *y = (pt.y - (rc.bottom - bh) * 0.5f) * 2160.0f / bh;
        return true;
    }

}
