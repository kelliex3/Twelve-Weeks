#include "text-input.hpp"

#include "window.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace engine::text_input {

    namespace {

        constexpr int kQueueCap = 96;

        bool        g_cur[256]{};
        bool        g_prev[256]{};
        Key         g_queue[kQueueCap]{};
        int         g_queueN = 0;
        std::atomic<int> g_owner{ static_cast<int>(Owner::None) };

        void Push(int vk, uint32_t ch)
        {
            if (g_queueN >= kQueueCap) return;
            g_queue[g_queueN++] = Key{ vk, ch, true };
        }

        // GetAsyncKeyState gives the virtual key, not the character. Translate the key
        // here with the live keyboard state so Polish / diacritics type correctly without
        // owning a hook. ToUnicode is only called while our window is focused (Down()
        // already checks that).
        void QueueKey(int vk)
        {
            BYTE ks[256]{};
            if (!GetKeyboardState(ks)) {
                Push(vk, 0);
                return;
            }
            const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
            WCHAR w[8]{};
            const int n = ToUnicode(static_cast<UINT>(vk), scan, ks, w, 8, 0);
            if (n <= 0) {
                Push(vk, 0);
                return;
            }
            for (int i = 0; i < n && i < 8; i++) {
                WCHAR cp = w[i];
                if (cp == 0) continue;
                if (cp >= 0xD800 && cp < 0xE000 && i + 1 < n) cp = w[++i];
                Push(vk, static_cast<uint32_t>(cp));
            }
        }

    } // namespace

    void BeginFrame()
    {
        g_queueN = 0;
        for (int vk = 0; vk < 256; vk++) {
            const bool now = engine::window::KeyDown(vk);
            g_prev[vk] = g_cur[vk];
            g_cur[vk] = now;
            if (now && !g_prev[vk]) QueueKey(vk);
        }
    }

    bool Down(int vk)
    {
        if (vk < 0 || vk >= 256) return false;
        return g_cur[vk & 0xff];
    }

    bool Edge(int vk)
    {
        if (vk < 0 || vk >= 256) return false;
        return g_cur[vk & 0xff] && !g_prev[vk & 0xff];
    }

    bool TakeEdge(int vk)
    {
        if (!Edge(vk)) return false;
        g_prev[vk & 0xff] = g_cur[vk & 0xff];
        return true;
    }

    bool Next(Key& out)
    {
        if (g_queueN <= 0) return false;
        out = g_queue[0];
        for (int i = 1; i < g_queueN; i++) g_queue[i - 1] = g_queue[i];
        g_queueN--;
        return true;
    }

    bool Paste(char* out, size_t cap)
    {
        if (!out || cap == 0 || !engine::window::Focused()) return false;
        out[0] = 0;
        if (!OpenClipboard(nullptr)) return false;
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        bool ok = false;
        if (h) {
            const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h));
            if (w) {
                const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out,
                    static_cast<int>(cap), nullptr, nullptr);
                GlobalUnlock(h);
                if (n > 0) {
                    out[cap - 1] = 0;
                    ok = true;
                }
            }
        }
        CloseClipboard();
        return ok;
    }

    bool Begin(Owner owner)
    {
        if (owner == Owner::None) return false;
        int expected = static_cast<int>(Owner::None);
        return g_owner.compare_exchange_strong(expected, static_cast<int>(owner));
    }

    void End(Owner owner)
    {
        int expected = static_cast<int>(owner);
        g_owner.compare_exchange_strong(expected, static_cast<int>(Owner::None));
    }

    bool Active() { return Holder() != Owner::None; }

    Owner Holder() { return static_cast<Owner>(g_owner.load()); }

    void Reset()
    {
        std::memset(g_cur, 0, sizeof g_cur);
        std::memset(g_prev, 0, sizeof g_prev);
        g_queueN = 0;
        g_owner.store(static_cast<int>(Owner::None));
    }

} // namespace engine::text_input
