#pragma once

#include "sid/sid.hpp"

#include <cstdint>

// Ported from tools/ext/t2r-ext.c's own-side backpack hand-off (feature_backpack_prop:
// BpMe / BpCtrlOf / BpState / BpWornSnapshot / BpWornOff / BpWornOn / the controller vtable
// hooks / the held leave). The worn bag <-> TAB prop swap runs inside the engine's own
// EnterBackpack / LeaveBackpack (the craft clip's "enter-backpack" / "leave-backpack"
// events): enter shows the prop, reparents the weapons and hides the look's bag mesh;
// leave hides the prop, reparents the weapons back and shows the bag mesh. The engine only
// knows the look's ORIGINAL bag sid, so the rigged copies / armour twins are ours to switch
// on the same call.
//
// This module is the engine access + the hand-off; the per-frame driver and the wire live in
// net/match/backpack.* (the ext's BpOwnTick / BpNetTick / BpPupTick).
//
// Ported here beyond the hand-off:
//   * the armour rig (engine/armor-rig.*): WornOn's pair re-derive and ReapplyRig() read the rig's
//     own mask / twin row / body-copy probe. The rig's per-frame DRIVER (feature_armor_visual) is
//     not ported, so the own-side rig stays unprobed and both are inert until that producer lands.
//   * the weapon reparent / attach (ext BpReparentHook / BpAttachHook): the two engine functions
//     are inline-hooked and our own bag-off's instant attaches are turned into the blended attach.
//     t2r-bp-weapons-body.txt = the opt-in old behaviour (weapons stay on the body).
//   * the per-look TAB prop table (ext BP_PROP_LOOKS / BpTables / BpLookProp) and the player-look
//     read (BpPlayerLook), exposed for net/match/backpack.*.
//   * the ps57 put-away-refusal recovery: CurrentWeapon() and the refusal-frame registry are here;
//     the per-frame recovery runs in net/match/backpack.cpp's OwnPoll. The refusal-frame PRODUCER
//     (the holster fix / sreq gate) is not ported, so the recovery is inert until it lands.

namespace engine::backpack_prop {

    // The engine's own TAB state (ext BpState): have = a controller was reachable; active =
    // the engine's [ctrl+0x190] (the bag is off, the prop is out); vis = the prop process's
    // visible byte (0/1; -1 = the prop process is not reachable). ctrl = the controller.
    struct State { int have; int active; int vis; uint8_t* ctrl; };
    State Query();

    // The local player process (ext BpMe = kFnLocalPlayer).
    uint8_t* Me();

    // Player+0x2f58 -> the PlayerBackpackAnimCtrl, validated (vtable + player back-link).
    // Re-read through the player every time: the engine's relocatable process heap moves
    // the player and its controller.
    uint8_t* CtrlOf(uint8_t* me);

    // Our own controller, direct route through Me(). (The ext's candidate-table / last-spawn
    // fallback is not ported: the direct route already re-reads every frame.)
    uint8_t* CtrlOwn();

    // ext g_bpHid: our worn bag parts are held off (the engine's prop is out). This is the
    // bag-off byte the wire reports.
    bool Hid();

    // ext g_bpWornHidden: the armour rig keeps the bag pair hidden while the prop is out
    // (engine/armor-rig's BodyApply reads it). This is our own side's bag pair.
    bool WornHidden();

    // Our life token (ext g_spawnedFrame): bumped once per local (re)spawn by BumpLife()
    // (GiveLocalLoadout is the mod's spawn give). A feature that holds per-life state records
    // it and voids the state when it changes. NOT the process pointer: the heap moves the
    // player mid-life.
    uint64_t LifeId();
    void     BumpLife();

    // Enter / leave counters and the QPC-ms of the last swap (ext g_bp_n_enter / g_bp_n_leave
    // / g_bp_t_enter / g_bp_t_leave). The wire sender reads them to report where the bag swap
    // sat in the owner's clip time. src: 1 = engine hook, 2 = per-frame poll of [ctrl+0x190],
    // 3 = menu fallback.
    uint32_t EnterCount();
    uint32_t LeaveCount();
    double   EnterMs();
    double   LeaveMs();
    int      EnterSrc();
    int      LeaveSrc();

    // Set by the per-frame driver: the level is live (ext g_bp_inmatch). The vtable hooks act
    // on our controller only while this is set.
    void SetInMatch(bool inMatch);

    // ---- the worn-part table (ext BP_PARTS / g_bpPartSid) --------------------------------
    // The net receiver hides / shows the same 23 bag parts on a puppet, so it reads the
    // table here rather than duplicating it.
    int        PartCount();
    StringId64 PartSid(int i);

    // The held state still belongs to the current life (ext g_bpHidGen == g_spawnedFrame).
    // false = a new body: the per-frame driver drops the held state.
    bool HeldGenOk();

    // Drop the held state without touching the meshes (ext: g_bpHid = g_bpWornHidden = 0).
    void ClearHeld();

    // Re-assert the hidden parts this frame (the ext's per-frame re-hide while g_bpHid: an
    // armour-rig pair flip or an engine re-instance must not bring a bag back).
    void HideWornNow(uint8_t* me);

    // ---- the worn-part swap (called by the hooks and the per-frame poll) ----------------

    // Snapshot which of the 23 bag parts the look has and which show (ext BpWornSnapshot).
    void WornSnapshot(uint8_t* me);

    // Hide every present bag part and remember the visible ones (ext BpWornOff). src as above.
    void WornOff(uint8_t* me, int src);

    // Show exactly the parts the snapshot says the look shows (ext BpWornOn). The armour-rig
    // branch re-derives `want` from the rig's pushed / original pair where the rig is live.
    void WornOn(uint8_t* me, int src);

    // ext BpReapplyRig: re-apply the armour rig's pushed / original pair of every body part of our
    // own twin from its last known mask (armor_rig::OwnLook / OwnWant / OwnBodyOkArr). A no-op
    // while the rig is unprobed (the armour-visual driver is not ported).
    void ReapplyRig();

    // ---- the TAB prop per-look table (ext BP_PROP_LOOKS / BpTables / BpLookProp) -----------
    // The look -> prop-look table (16 rows; the ext's dc_data_patches.BACKPACK_PROPS). net/match/
    // backpack.* reads it instead of duplicating the table.
    int         PropLookCount();                 // ext BP_NPROP
    const char* PropLookName(int i);             // ext BP_PROP_LOOKS[i][1] (the prop look name)
    StringId64  PropArtSid(int i);               // ext g_bp_prop_sid[i] (the prop look sid)
    // ext BpLookProp: the table row that serves this look sid (-1 none / off). A row is served when
    // it is one of the "always" rows (the shared scar / militia-11 props) or the faction-look
    // feature is on (t2r-faction-looks.txt).
    int      LookProp(uint64_t look);
    // ext BpPlayerLook: the look sid of a player process (vtable +0x390, as the engine's prop pick
    // reads it). 0 = none.
    uint64_t PlayerLook(uint8_t* player);
    bool     PerLook();                          // ext g_bpPerLook (default on)
    void     SetPerLook(bool on);

    // ---- the weapon reparent / attach (ext BpReparentHook / BpAttachHook) ------------------
    // ReparentWeapons (bag off: toBag 1) puts the stowed weapons on the prop's targRifle /
    // targRifle2 / melee points; the way back already blends. Our own toBag 1 attaches are turned
    // into the engine's blended attach (0.2 s) so the gun leaves the sling on the very call the
    // bag swaps. The two engine functions are inline-hooked by Install().
    bool WpnBody();                              // ext g_bpWpnBody (t2r-bp-weapons-body.txt)
    int  WpnHooks();                             // 2 = both weapon hooks live (ext g_bpHooks bit 1)

    // ---- the ps57 put-away-refusal recovery (ext g_bp57RefuseF / CurrentWeapon) -------------
    // The frame of the last put-away refusal (set by the unported holster fix / sreq gate). The
    // per-frame driver in net/match/backpack.cpp holsters instantly when the menu came up and the
    // engine's bag never did, a refusal came right before it, and the weapon is still held.
    void     NoteRefuse(uint64_t frame);
    uint64_t RefuseFrame();
    // ext CurrentWeapon: get-player-desired-or-current-weapon-id (a typed binding: rec+8 = a
    // descriptor, rec+0x10 = the impl). ~0 = none / no player.
    uint64_t CurrentWeapon();

    // ---- the held leave ------------------------------------------------------------------

    // The leave hand-off hold (ext BpLeaveHoldMs / t2r-bp-leave-delay.txt): ms, 0..1500,
    // default 150. The engine's leave event is held back so the prop bag finishes its outro
    // with the weapons still riding it.
    int  LeaveHoldMs();
    bool LeavePending();
    void RunPendingLeave(bool menu);   // ext BpLeaveRunPending

    // ---- install / teardown ---------------------------------------------------------------

    // Patch the controller vtable's enter / leave slots (idempotent, safe every frame) and
    // inline-hook the weapon reparent / attach functions (0xafc740 / 0x1cd2140). Logged once.
    void Install();

    void Reset();

}
