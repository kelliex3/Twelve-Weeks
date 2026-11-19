# bloodbuild port

Port of `tools/ext/bloodbuild.c` (996 LOC) plus the dependencies it needs, so other mod code
can use them. The ext C stays the behavioural reference; nothing there is moved or deleted.

Scope chosen by the user: **full live port**. bloodbuild is a purely local, cosmetic feature
(no network message): a per-body `blood` / `dirt` level driven by events every client already
sees identically (synced health, paired moves, down / death, the downed crawl, the life
counter), escalating into real engine blood-mask / particle effects through the engine's own
`part-effect` event path.

## Status: ported (in progress for cutover)

The whole feature is folded into `net/match/bloodbuild.{hpp,cpp}` and ticked from
`loader/boot.cpp` (`FrameTick`). The dependency modules below are reusable by any other mod
code. The ext keeps running unchanged as the bridge; the ext's own `t2r-*.txt` toggles still
gate both copies, so cutover is per-feature as with the other ports.

## Honest limitation

bloodbuild reads three **private generated tables** (`tools/ext/GENERATED.md`):
`CTX_FX` / `KNIFE_HIT_FX` (`ctx_fx_events.h`) and `FIN_ANIMS` (`finisher_anims.h`). Those
generators are not shipped, so the tables cannot be produced locally. `BbInit()` sets
`g_bb_nfx` from them and `BloodbuildTick()` returns early while `g_bb_nfx == 0`, so with no
tables the whole feature is inert. This port keeps that gate faithfully behind
`T2R_DATA_CTX_FX`: define it (and drop the generated headers next to the feature) to go live.
Without it the port compiles and behaves exactly as the ext does with an empty table set.

## File homes

| Module | Home | Ext reference |
|---|---|---|
| Clip `part-effect` fire path | `engine/clipfx.{hpp,cpp}` | `clipfx.c`: `CfxeEntry` / `CfxeCheck` / `CfxeFile` / `ClipfxOff` / `CfxeObjOf` / `CfxeSafeJoint` / `ClipfxFire` |
| Particle spawn + compute entries | `engine/vfx` (extended) | `t2r-ext.c`: `CfxSpawnRaw` / `ArmorFxSpawn` / `CfxStream` / `CfxRtmgr` / `CfxUsed` / `CfxMakeRoom` / `CfxRetryTick` |
| Joint finiteness guard | `engine/object` (extended) | `t2r-ext.c`: `CfxJointFinite` |
| 2013 melee move table | `gameplay/melee2013.{hpp,cpp}` | `t2r-ext.c`: `MELEE2013` / `MxMove` / `MxByMove` |
| Contextual-down pair table | `gameplay/ctxdown.{hpp,cpp}` + `gameplay/ctxdown-rows/*.h` | `ctxdown.c`: `CD_PAIRS` / `CdSegLen` (+ `mduel_*.h` / `pullout_rows.h`) |
| Paired-move surface | `net/anim/paired.{hpp,cpp}` | `paired.c` `g_ps` (the fields bloodbuild reads) + `PsyncMs` / `PsyncAbort` |
| The feature | `net/match/bloodbuild.{hpp,cpp}` | `bloodbuild.c` |
| Tick wiring | `loader/boot.cpp` `FrameTick` list | `bloodbuild_tick()` call |

## Deviations (documented, faithful where it matters)

* **FireEffect watch hook deferred.** `clipfx.c`'s `install_clipfx_hook` (the engine-vs-ours
  verdict that avoids double blood) is not needed by bloodbuild and is not ported here. `Fire`
  therefore fires every event; if the ext's hook is also installed the slot points outside the
  exe and `Fire` refuses (no double blood), matching the ext's own guard.
* **psync bridge.** The mod does not run the ext's `paired.c`. `net/anim/paired` exposes the
  `g_ps` shape bloodbuild reads and is populated from `gameplay::melee` at execution
  start / end. The mod's own execution move (`fist-throat-punch-kill`) is not a 2013 move, so
  the generic pair path runs: victim blood / wound, attacker splatter.
* **Abort wiring.** The ext hard-wires `BbCancelPair` inside `paired.c`'s `psync_abort` (only
  for a `PS_CTXDOWN` move whose abort reason is a dodge / refusal). The mod keeps
  `gameplay/melee` decoupled from the feature: `net::anim::paired::SetAbortHandler` registers
  `BbCancelPair`, and `paired::End(aborted)` fires it. `gameplay::melee`'s
  `FinishExecution` / `FinishBeingExecuted` forward `aborted`, set at the three abort sites
  (victim puppet destroyed, target gone, timeout). The ext's `PS_CTXDOWN` guard is not
  reproduced (the mod's execution is `kMeleeExec`), a faithful-approximation deviation.
* **Private tables.** `CTX_FX` / `KNIFE_HIT_FX` / `FIN_ANIMS` are empty unless
  `T2R_DATA_CTX_FX` is defined. `MeleeBladeKills` (`MeleeRuleBlade` + `!IntLive()`, the
  `melee_hitfx` weapon-class rule) is an honest TODO stub returning false, and the downed
  state reads `player_status::LocalDowned` / `PuppetDowned` (the ext's dying `2` distinction
  is not ported), so the blade-finisher lethality path and the crawl state stay inert until
  those subsystems land.
* **In-level gate.** The ext tests `MatchSubstate() == 6`; the port uses the level phases
  `[LoadingPlaceSpawn, Live]`. Harmless: bodies do not exist during loading.
* **Log prefix** is `[BLOOD]` (ext: `blood: `).
