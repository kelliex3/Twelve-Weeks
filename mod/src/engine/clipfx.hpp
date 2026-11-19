#pragma once

#include "sid/sid.hpp"

#include <cstdint>

// Ported from tools/ext/clipfx.c: TLOU2's own anim `part-effect` events fired through the
// ENGINE's event path. A takedown / melee blood effect (blood streams, ribbons, sprays, wall
// splatters, skin blood masks) is a `part-effect` anim event (type 0xc0d9313c7f55b23e): a
// {type, frame, count, params {key, value}} entry the clip carries. The engine fires them
// itself for the clips it plays; a feature that wants an SP-exact effect on a body it drives
// (a psync puppet, our own half) builds the same call the engine's NdGameObject::HandleEvent
// default case makes.
//
// This module is the fire path only: the entry layout, the exe code check, the sidecar
// toggles, the handle -> object lookup and the non-finite-joint guard. The ext's FireEffect
// watch hook (the engine-vs-ours verdict that avoids double blood) is NOT ported -- bloodbuild
// does not need it. `Fire` therefore fires every event; when the ext's hook is also installed
// the vtable slot points outside the exe and `Fire` refuses, which is the ext's own guard.

namespace engine::clipfx {

    // The clip's effect event. 24 bytes, laid out exactly as the engine reads it.
    struct Entry {
        uint64_t type;                  // 0xc0d9313c7f55b23e
        float    frame;
        uint32_t n;
        const uint64_t (*pairs)[2];     // {key, value} params
    };
    static_assert(sizeof(Entry) == 24, "anim effect event entry is 24 bytes");

    inline constexpr uint64_t kType = 0xc0d9313c7f55b23eULL;

    // t2r-clipfx-off.txt next to the exe: the old spawn-particles-at-joint path, no clip
    // event. Cached 2 s (ext ClipfxOff / CfxeFile).
    bool Off();

    // The ext's generic cached sidecar test (CfxeFile): `name` next to the exe, re-checked
    // at most every 2 s. `*cache` starts at -1.
    bool File(const char* name, int* cache, uint64_t* chk);

    // ent (SID_PLAYER or a body handle) -> the body process. nullptr = not found (ext
    // CfxeObjOf).
    uint8_t* ObjOf(uint64_t ent);

    // Fire one clip event on `obj` exactly as NdGameObject::HandleEvent's default case does.
    // ownBuilder: true = the object's own effect-info builder (vtable +0x508; correct for our
    // own player), false = the base builder (used for puppets: no remote-player state touched).
    // Returns the FireEffect result's first word (0 on refusal). ext ClipfxFire.
    uint64_t Fire(uint8_t* obj, uint64_t clip, const Entry* e, bool ownBuilder);

    // The exe code check (ext CfxeCheck): true once, cached. false = the engine path cannot be
    // trusted and `Fire` refuses (the caller falls back to plain spawn-particles).
    bool Check();

}
