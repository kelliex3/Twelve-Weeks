#pragma once

#include "sid/sid.hpp"

#include <cstdint>

// Ported from tools/ext/t2r-ext.c's object helpers (XbxLoc / GvisDc / ObjLook / LookName /
// SetObjLoc). Entity-level reads a feature needs on a body or a spawned prop, kept out of the
// net layer so any engine-side feature can use them.
//
// The ext reached the engine through kFnEntityFromHandle (a handle -> process lookup); the mod
// already has the same route through DcManager::LookupObject, which these helpers use. The
// process world read itself is engine::transform::ProcessWorld (exe+0x133DB30).

namespace engine::object {

    // World position (out[0..2]) and rotation (out[4..7], xyzw) of a body / prop process.
    // `handle` is a StringId64 (the local player is SID("player")). false = no process or
    // the engine returned nothing.
    bool EntityWorld(StringId64 handle, float out[8]);
    bool LocalPlayerWorld(float out[8]);

    // ext XbxLoc: world position (p[3]) and rotation (q[4], xyzw) of a body's joint locator.
    // `q` may be null. false = no joint locator / unreadable / non-finite.
    bool JointLocator(StringId64 entity, const char* joint, float p[3], float q[4]);

    // ext CfxJointFinite: does this joint's world position read finite on this body? A joint
    // that does not exist counts as true (the spawn natives deal with that themselves); a
    // joint whose locator reads NaN / huge counts as false (the engine's particle spawn
    // asserts IsFinite). Used before spawning a joint-bound particle on a body posed by a
    // paired clip.
    bool JointFinite(StringId64 chr, StringId64 jsid);

    // ext GvisDc: the draw control ([proc+0x1B0]) of a body process, with the ext's dead
    // guard (a released vtable is refused). nullptr when there is none.
    uint8_t* DrawControl(StringId64 handle);
    uint8_t* DrawControlOfProc(uintptr_t proc);   // ext BpDc (a process pointer in hand)

    // ext BpDcFind / BpDcHidden / BpDcHide / BpDcShow: a draw control's mesh table by sid.
    // The TAB hand-off hides the worn bag parts the engine's own enter does not know about
    // (the rigged copies / armour twins) and re-asserts them while the prop is out.
    // MeshIndex: -1 = the mesh is not on this draw control. MeshHidden is only meaningful
    // for an index >= 0. Hide/ShowMesh take the sid (they resolve the index themselves).
    int  MeshIndex(uint8_t* dc, StringId64 sid);
    bool MeshHidden(uint8_t* dc, int idx);
    void HideMesh(uint8_t* dc, StringId64 sid);
    void ShowMesh(uint8_t* dc, StringId64 sid);

    // ext ObjLook: get-object-look(o, 0) -- the look sid a process wears. 0 / ~0 = none.
    uint64_t Look(StringId64 object);

    // Pose an object at a world position / rotation (a bound frame through
    // set-object-locator). Non-finite / out-of-range inputs are refused.
    void SetLocator(StringId64 handle, const float pos[3], const float quat[4]);

    // ext LookName: a diagnostic name for a look sid, from tlou-ii-mp\t2r-looks.txt next to
    // the exe. "?" when the table has not loaded or the sid is unknown. Log-only.
    const char* LookName(uint64_t sid);

}
