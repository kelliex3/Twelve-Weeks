// ============================================================================
//  anim.hpp -- public surface of the anim-replication engine, plus the three
//  entry points the loader/menu wiring calls:
//
//    NetPatch()      called by the loader BEFORE the game runs
//                    (defined in net-patches.cpp)
//    NetMenu()       called once at menu build (defined in net-menu.cpp)
//    NetTick()       called every frame on the game thread
//                    (defined in match/match.cpp)
// ============================================================================
#pragma once

#include <cstdint>   // uint16_t / uint32_t / uint64_t used in the anim-replication decls below

// Set by net::menu::LoadMpAssets (the match-load flow), cleared by
// ResetMpAssetState -- gates all per-frame net work. Means "MP assets loaded +
// hooks installed", not "in a multiplayer session" (see net-menu.cpp).
extern bool multiplayer;

void NetMenu();
void NetTick();

// The anim engine's early frame steps (frame-clock publish, snapshot-node
// service, anim-hook install), defined in anim/anim-hooks.cpp. The driver
// (NetTick, match/match.cpp) calls this FIRST each frame.
void NetAnimTickEarly();

// ---- NPC-puppet anim replication -- full AnimCmdList replay ----------------
// SENDER: capture the local player's exact inner AnimCmdList (clip pointers
// rewritten to StringId64 names) + aim spine into the outgoing state.
// RECEIVER: replay that command list onto the remote NPC inside its own
// CreateAnimCmds (resolving names -> local anims, re-deriving feather masks).
// Aim is copied post-anim either way. Install() wires both engine hooks once.
//   spine20 = float[20]. There is no base-clip fallback: animate_ would start a real
// anim state whose baked root motion drags the puppet against the replicated locator.
// The AnimCmdList itself rides in its own packet (see NetGet/NetSetRemoteAnimCmds).
void NetCaptureLocalAnim(float* spine20);
// PER REMOTE SLOT (0..kMaxNetPlayers-1): each remote puppet has its own replay state, so a 2v2's
// three remotes each animate from their own replicated list. slot = the ORIGINAL author's net slot.
void NetApplyRemoteAnim(int slot, uint64_t npcSid, const float* spine20);
// Publish one puppet's replicated weapon-IK feather fade (0..1). GAME THREAD, once per
// frame per remote, alongside NetApplyRemoteAnim.
//   The puppet cannot work this out for itself. On the PLAYER the engine runs weapon grip
// IK by calling WeaponIKFeather::Apply directly and emits no anim command, so the captured
// list never contains it; on an NPC it comes from the anim-state layer walk, which our
// replay replaces wholesale, so ND's own emitter never runs at all. Without this the
// puppet's arms keep the pose the sender
// had with no correction onto the weapon it is actually holding: the residual
// "holding it wrong" after the attach point itself is correct.
//   0 = weapon stowed -> no arm solve at all (this is also the reset value).
void NetSetPuppetWeaponIk(int slot, float fade);

// Immediately stop ONE puppet's anim replay: clears its drive pointers so the CreateAnimCmds /
// PostAnim hooks no longer recognise (and write into) that puppet's AnimControl. Call this the
// instant a puppet is dead / being killed / respawned -- continuing to replay onto a dying or
// freed NPC's joint stack corrupts memory and crashes. NetClearRemoteAnimAll() stops every puppet.
// dropLastGood=true (the default) also forgets the pose the puppet was holding, so a slot handed
// to a DIFFERENT peer can never replay the previous occupant's pose. Pass false when the puppet
// objects are merely being re-created for the same peer (our own respawn) -- the replicated list
// belongs to the sender's rig, not to our death, and wiping it snaps every remote to bind pose.
void NetClearRemoteAnim(int slot, bool dropLastGood = true);
void NetClearRemoteAnimAll(bool dropLastGood = true);

// Kill ONE puppet's two post-anim overlays (replicated aim spine + weapon IK) WITHOUT
// touching its drive pointers. This is what a DEATH needs and Detach is not: the replay
// itself is what puts the replicated pose under the ragdoll, so npcAC must survive -- but
// hk_PostAnim runs AFTER the engine's PostAnimBlending, i.e. after the ragdoll has written
// its own joints, and neither overlay can ever be refreshed again once ApplyRemote stops
// running for a corpse. Leaving the aim on stamps a frozen pre-death spine over a
// ragdolling body every frame. GAME THREAD.
void NetFreezePuppetOverlays(int slot);

// Per-slot "the owner is still sending us STATE packets" flag, published ONCE PER FRAME from
// the GAME THREAD (replication::Tick) and read lock-free by the anim thread's release ladder.
// A stalled ANIM stream and a departed PLAYER are different things: the ladder used to treat
// them the same and hand the puppet back to its native anim-state machine, which script mode
// does not stop -- so the NPC resumed idling, looking and aiming. That was invisible while the
// puppet was parked on `zen` (its "native idle" WAS zen); with zen gone it is the visible
// "it reacts once the anim sync stops". Release only when the slot itself is really gone.
void NetSetSlotStreamAlive(int slot, bool alive);

// ---- POSE INTERPOLATION (the smoothing) -----------------------------------------------
// Puppets change pose only when a packet lands, and are frozen in between, while their root
// transform is smoothly lerped - two clocks on one body, which is the visible "snapping".
// With this on, each DISTINCT received pose is translated exactly once into one of two stores
// and every frame merely restores both and blends by a playback clock running 25 ms behind the
// newest sample. Steady state is 14 dwords with no clip lookups - cheaper as well as smoother.
// Requires protocol v29 (AnimCmdPayload::sendMs) on both ends.
// This switch also gates snapshot CAPTURE, so turning it OFF stops both.
extern volatile long g_animInterp;

// ---- puppet anim effects + overlay remap ---------------------------------------------------
// g_animEffects:    regenerate footsteps/foley/weapon events on puppets from the clips we already
//                   replay. Effects fire from AnimControl::Step off the puppet's OWN (idle) anim
//                   state, before our pose replacement, and are not carried in the AnimCmdList at
//                   all; this switch regenerates the correct ones from the replayed clips.
//                   (Muting the puppet's own effects instead is a crash-documented dead end;
//                   see docs/engine-notes.md.)
// g_txOverlayRemap: resolve the anim OVERLAY before reading a clip's identity at capture, as ND
//                   does. Leading suspect for the residual clip-resolve failures.
extern volatile long g_animEffects;      // regenerate puppet effects (footsteps/foley/weapon)
extern volatile long g_txOverlayRemap;

// KILL SWITCH for the whole AnimCmdList pipeline (capture hook, replay, packets,
// aim spine copy). Set false to take it completely out of the picture -- the engine
// hooks are not even installed -- so a crash can be attributed to it or ruled out.
// Flipping it back to true at runtime installs the hooks on the next frame.
extern bool g_animSyncEnabled;

// AnimCmdList transfer (net-session owns the packet; the anim engine owns the buffers).
//   NetGetLocalAnimCmds  -- SENDER: copy the latest captured list out. Returns dwords (0 = none).
//   NetAnimRxIngest      -- RECEIVER, step 1: the per-author sequence gate. Called by the
//                          session's HandleAnimCmd BEFORE payload parse/size validation.
//                          Returns false for a reordered/duplicate packet -- the caller must
//                          drop it and, on the host, must NOT relay it (drop-before-relay).
//                          The -1024 restart window and the baseline reset tiers are
//                          documented at the definition (anim-rx.cpp).
//   NetSetRemoteAnimCmds -- RECEIVER, step 2: decode/validate/publish the accepted list to the
//                          puppet replay hook.
// skelId gates replay: the engine soft-fails a rig mismatch to an empty pose, which
// is precisely the input that poisons a one-sided blend, so a differing rig must not
// replay at all. 0 from the getter means "nothing to send this frame".
// The list is BIT-PACKED on the wire (~1424 B raw -> ~250 B, bit-exact -- see the codec
// in anim-codec.cpp). Get returns BITS written; a raw-dword fallback covers any list the
// packer can't express, so the codec can cost size but never a frame.
uint16_t NetGetLocalAnimCmds(uint8_t* out, uint16_t maxBytes,
    uint32_t* outSkelId, uint8_t* outEpoch);
bool     NetAnimRxIngest(int authorSlot, uint32_t seq);
void     NetSetRemoteAnimCmds(int senderSlot, const uint8_t* data, uint16_t bits, uint32_t skelId, uint8_t epoch,
    uint32_t sendMs);   // sendMs: author's capture time (v29), the interp time base
// FULL reset -- TX re-define + wipe EVERY sender's rx dictionary + replay banks + the rx
// sequence baselines. Use ONLY on full session teardown (LeaveInternal). For incremental
// events use NetAnimTxReset / NetAnimClearSlot below, which never wipe a co-present
// converged sender's learned dict.
void     NetAnimDictReset();
// Clear the per-session anim residuals no reset tier owns: the per-slot effect phase caches
// (AnimReplay fields, applied on the anim thread via the deferred-clear protocol) and the
// local skeleton pointer (g_localSkel: stale across level loads). Called from session
// teardown at the same point NetAnimDictReset runs.
void     NetAnimResetSessionResiduals();
// TX-only re-definition for incremental lifecycle events (peer joins, roster grows, keepalive
// re-Hello): re-defines our own clips to all receivers via an epoch bump; touches no rx dict.
void     NetAnimTxReset();
// Clear one sender's replay bank + learned rx dictionary + rx sequence baseline (peer
// dropped / that slot restarted).
void     NetAnimClearSlot(int slot);
// Freshness stamp of the last ACCEPTED remote anim list for a slot (g_nowMs time base,
// GetTickCount64() & 0x7fffffff). 0 = none. Used by the ghost-peer reaper as the mod's
// stand-in for the ext's per-slot puppet pose-write counter.
uint32_t NetAnimRxStampMs(int slot);
void NetPuppetAnimInstall();