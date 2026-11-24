#pragma once

#include <cstddef>
#include <cstdint>

// One shared, hook-free keyboard sampler for every native text field (chat, the
// FRIENDS add-name line). The ext's gearup13.c owns the process-wide WH_KEYBOARD_LL
// hook and a raw-input thunk while a line is typed; adding a native hook would be a
// second unmanaged keyboard hook, so this module deliberately uses only polled Win32
// state plus ToUnicode while the game is focused. It also owns the one "a text field
// is active" gate, so two panels cannot both claim the keyboard.
//
// BeginFrame() is called once from boot::FrameTick, before any panel Tick(). Panels
// then read Down/Edge and drain Next() for printable characters.

namespace engine::text_input {

    enum class Owner : uint8_t {
        None   = 0,
        Chat   = 1,
        Friends = 2,
    };

    struct Key {
        int      vk = 0;
        uint32_t ch = 0;      // translated Unicode point, 0 for control / unmapped keys
        bool     down = true; // currently only rising edges are queued
    };

    void BeginFrame();

    bool Down(int vk);
    bool Edge(int vk);
    // Read and clear one key's rising edge. Used for keys that must not also reach the
    // menu (chat's Enter-to-open / Enter-to-send).
    bool TakeEdge(int vk);

    bool Next(Key& out);

    // UTF-8 clipboard paste, shared by every editor. false = no text / no focus.
    bool Paste(char* out, size_t cap);

    // Only one owner may hold the typing gate; Begin returns false for a second one.
    bool Begin(Owner owner);
    void End(Owner owner);
    bool Active();
    Owner Holder();

    void Reset();

} // namespace engine::text_input
