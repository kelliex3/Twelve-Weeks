#pragma once

#include <cstddef>
#include <cstdint>

// Native owner of the low-level GUI2 widget surface the death screen needs (ported
// from tools/ext/t2r-ext.c's G2Ok / G2Find / G2Name and death_ui.c's HUD-under-black
// walk). The engine's script wrappers only resolve a known path; they cannot walk the
// canvas children or read a widget's current alpha, which is what fading the HUD
// under the death screen's black requires.
//
// Every entry point validates the pinned function prologue once and every returned
// widget pointer with engine::ptr's VirtualQuery guards. A game update that moves an
// RVA therefore fails closed (Available() == false) instead of calling a moved address.

namespace engine::gui2_low {

    // All pinned RVAs have their expected prologue. Enumerate children only when true.
    bool Available();

    // gui2 system -> widget object for a baked path, or 0. `path` is a SID.
    uintptr_t Widget(uint64_t path);

    // The engine-side name of a widget object ("hudMenuStack", ...), "?" on failure.
    const char* Name(uintptr_t widget);

    // The widget's own draw alpha (ext widget + 0x3bc), 0 on an unreadable object.
    float Alpha(uintptr_t widget);

    // FUN_141e5b4d0(widget*, alpha): the setter behind gui2-set-widget-alpha.
    void SetAlpha(uintptr_t widget, float alpha);

    // First child / next sibling of a widget path (ext RVA_GUI2_CHILD/SIBLING).
    // Both write 0 on failure; NextSibling returns the parent itself at the end of
    // the list, which is the caller's stop condition.
    void FirstChild(uint64_t parentPath, uint64_t* out);
    void NextSibling(uint64_t path, uint64_t* out);

    // A legacy DAT_ pointer stored in the game image (0x180189b90 / 0x180189ba0),
    // rebased onto this process. Returns 0 when the slot is unreadable.
    uint64_t LegacyHandle(uintptr_t imageVa);

    // Drop the cached prologue check (test / level reload helper).
    void Reset();

} // namespace engine::gui2_low
