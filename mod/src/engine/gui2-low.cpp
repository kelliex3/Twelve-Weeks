// ported from tools/ext/t2r-ext.c (G2Ok / G2Find / G2Name) and death_ui.c's
// HUD-under-black child walk. READ/WRITE against engine-owned GUI2 widgets.

#include "gui2-low.hpp"

#include "engine/ptr.hpp"

#include <windows.h>

#include <cstring>

namespace engine::gui2_low {

    namespace {

        constexpr uintptr_t kRvaGetSystem = 0x1270A80;    // FUN_141270a80(23)
        constexpr uintptr_t kRvaFind      = 0x1E8F190;    // FUN_141e8f190(system, path)
        constexpr uintptr_t kRvaChild     = 0x1E8F5E0;    // (u64* out, path)
        constexpr uintptr_t kRvaSibling   = 0x1E8F700;    // (u64* out, path)
        constexpr uintptr_t kRvaSetAlpha  = 0x1E5B4D0;    // (widget*, float)
        constexpr uintptr_t kImageBase    = 0x140000000;  // T2R_IMAGE_BASE
        constexpr int       kGui2SystemId = 23;
        constexpr size_t    kWidgetBytes  = 0x3C0;
        constexpr size_t    kAlphaOffset  = 0x3BC;

        uintptr_t Base()
        {
            return reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        }

        // The prologue bytes the ext pinned (HOOK_MISS on mismatch). All six live
        // in the same exe build as the rest of the mod's RVA surface.
        struct Sig { uintptr_t rva; uint8_t n; uint8_t b[10]; };
        constexpr Sig kSigs[] = {
            { kRvaGetSystem, 9,  { 0x8B, 0xC1, 0x48, 0x8D, 0x0D, 0x27, 0x01, 0xFE, 0x02 } },
            { kRvaFind,      10, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xDB, 0x48, 0xB8 } },
            { kRvaChild,     10, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30 } },
            { kRvaSibling,   10, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30 } },
            { kRvaSetAlpha,   8, { 0xC5, 0xFA, 0x10, 0x91, 0xBC, 0x03, 0x00, 0x00 } },
        };

        int g_ok = -1;

    } // namespace

    bool Available()
    {
        if (g_ok < 0) {
            const uintptr_t base = Base();
            g_ok = 1;
            for (const Sig& s : kSigs)
                if (!engine::ptr::Readable(base + s.rva, s.n) ||
                    std::memcmp(reinterpret_cast<const void*>(base + s.rva), s.b, s.n)) {
                    g_ok = 0;
                    break;
                }
        }
        return g_ok != 0;
    }

    uintptr_t Widget(uint64_t path)
    {
        if (!Available() || !path) return 0;
        using SystemFn = uintptr_t(__fastcall*)(int);
        using FindFn   = uintptr_t(__fastcall*)(uintptr_t, uint64_t);
        const uintptr_t base = Base();
        const uintptr_t sys = reinterpret_cast<SystemFn>(base + kRvaGetSystem)(kGui2SystemId);
        if (!sys) return 0;
        const uintptr_t w = reinterpret_cast<FindFn>(base + kRvaFind)(sys, path);
        return engine::ptr::Writable(w, kWidgetBytes) ? w : 0;
    }

    const char* Name(uintptr_t widget)
    {
        if (!widget || !engine::ptr::Readable(widget, sizeof(uintptr_t))) return "?";
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(widget);
        if (!engine::ptr::Readable(vt, 6 * sizeof(uintptr_t))) return "?";
        using NameFn = const char* (__fastcall*)(uintptr_t);
        const NameFn fn = reinterpret_cast<const NameFn*>(
            reinterpret_cast<const uint8_t*>(vt))[5];
        if (!engine::ptr::Readable(reinterpret_cast<uintptr_t>(fn), 1)) return "?";
        const char* n = fn(widget);
        return n && engine::ptr::Readable(reinterpret_cast<uintptr_t>(n), 1) ? n : "?";
    }

    float Alpha(uintptr_t widget)
    {
        const uintptr_t p = widget + kAlphaOffset;
        return engine::ptr::Readable(p, sizeof(float))
            ? *reinterpret_cast<const float*>(p) : 0.0f;
    }

    void SetAlpha(uintptr_t widget, float alpha)
    {
        if (!Available() || !widget) return;
        using SetAlphaFn = void(__fastcall*)(uintptr_t, float);
        reinterpret_cast<SetAlphaFn>(Base() + kRvaSetAlpha)(widget, alpha);
    }

    void FirstChild(uint64_t parentPath, uint64_t* out)
    {
        if (!out) return;
        *out = 0;
        if (!Available()) return;
        using WalkFn = void(__fastcall*)(uint64_t*, uint64_t);
        reinterpret_cast<WalkFn>(Base() + kRvaChild)(out, parentPath);
    }

    void NextSibling(uint64_t path, uint64_t* out)
    {
        if (!out) return;
        *out = 0;
        if (!Available()) return;
        using WalkFn = void(__fastcall*)(uint64_t*, uint64_t);
        reinterpret_cast<WalkFn>(Base() + kRvaSibling)(out, path);
    }

    uint64_t LegacyHandle(uintptr_t imageVa)
    {
        const uintptr_t p = Base() + (imageVa >= kImageBase ? imageVa - kImageBase : imageVa);
        return engine::ptr::Readable(p, sizeof(uint64_t))
            ? *reinterpret_cast<const uint64_t*>(p) : 0;
    }

    void Reset()
    {
        g_ok = -1;
    }

} // namespace engine::gui2_low
