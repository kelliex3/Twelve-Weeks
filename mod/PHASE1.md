# Phase 1 — Enabling infrastructure

Phase 0 ([FEATURES.md](FEATURES.md)) listed the 135 feature modules and tagged each with the
mechanisms it sits on. Phase 1 ports those **mechanisms** — once each, in `mod/` idiom — before any
feature is ported. A feature port is then "use an existing mechanism + write the behaviour", not
"re-invent native invocation / widget spawn / PCM playback every time".

Rule of this phase (from the Great Merge reframe): the ext module is the **behavioural reference**,
never code to move. We reimplement the mechanism natively; where a mechanism already exists in the
mod, the deliverable is *verification + extension*, not a rewrite.

## Mechanism status

| # | Mechanism | Ext reference | `mod/` today | Gap |
|---|---|---|---|---|
| 1 | Native invocation (DC lookup → call) | `DcCall` / `*(rec+0x10)` | **exists** — `DC::Call`, `DC::Call_V2` | cached-native helper; convention doc |
| 2 | Net event channel | `SendEvent` (winmm `0x18005DC60`, opcodes `0x41..0xAF`) | **exists** — `NetEventType` + `net::session::SendEvent` | slot reservation + exact-byte compatibility |
| 3 | Widgets / HUD | `GUI2_FIND/CHILD`, `gui2-rxfm-*` | **partial** — `GUI2::SPAWN_WIDGET`, `MENU2::*` | widget-tree nav, transform, draw helper, `mod/widgets/` source |
| 4 | PCM audio mixer | `waveOut*` in `loadseq.c`, `t2r-ext.c` | **partial** — `engine/pcm.*` (stream + voices + gain model); stings in `net/audio/*` | 3D voices, loop / crossfade, data wiring |
| 5 | Camera control | render-cam switch + `*_tracks.h` | **partial** — `CAMERA::SET_CAMERA_POSITION/OVERRIDE_FOV` | render-camera path + track evaluator |
| 6 | Pad injection | `padinject.c` (writes `Pad` before `Joypad::Update`) | **ported** — `engine/pad.*` (virtual presses / sticks via mid-hooks at rel32-scanned call sites; command-bit learning; activity stamp; dev-bot consumers still unported) | call-site scan needs live-exe verification |
| 7 | Generated data pipeline | 78 headers (`tools/ext/GENERATED.md`) | **partial** — `mod/cmake/*.py`, `mod/scripts/make-xvag.py` | wire ext generators / stub strategy |
| 8 | Config, toggles, logging | 1,636 `t2r-*.txt` + `xlog` | `TO1_LOG` + compile-time `config.hpp` | toggle policy + runtime config |
| 9 | Engine RVA call / hook | `*(exe + rva)(…)` + hand-built RWX trampolines (`bp_pick`, `padinject.c`) | **partial** — `engine/transform.*`, `engine/pcm.*`, `engine/backpack-prop.*` call pinned RVAs; `net-patches.cpp` / `loader/boot.cpp` / `engine/pad.cpp` hook via safetyhook | pattern resolution + a shared exe-call helper + version guard |

---

## 1. Native invocation — exists, verify and harden

**Ext mechanism.** `rec = FUN_18000a190(sid)` (the DC manager lookup); the native lives at
`*(rec + 0x10)`; call `native(&ret, a0..a7)` with 64-bit DC values. This is the ext's hot path —
it caches the record and calls raw.

**Mod today** (`src/engine/script.hpp`): the same idea, already typed.
- `DcManager::Lookup(SID)` → `DcValue*`; `SCRIPT_FUNC_INDEX == 1` → `val[1]` **is** `rec+0x10`.
- `DC::Call<"name">(args...)` packs up to 64 slots and calls `val[1]` — equivalent to the ext's path.
- `DC::Call_V2<"name">(args...)` calls `val[2]` with the `DcValue* ret, u64 x8` signature — the exact
  `native(&ret, a0..a7)` shape the ext uses for its second-style natives.
- `DcManager::LookupInModule`, `LookupObject`, `MEMORY::*`, `LOCATOR::*`, `ENTITY::*`, `CAMERA::*`,
  `AUDIO::*`, `GUI2::*`, `MENU2::*` already wrap the common natives.

**Gap.** Only a cached-native accessor for hot loops (the ext looks a record up once and calls it
every frame; `DC::Call` re-looks-up each call). Nothing is missing structurally.

**Deliverable.** A small `DC::Native<"name">` (lookup once → hold `val[1]`/`val[2]` fn pointer →
`operator()`) plus a short section in `PORTING.md` documenting the mapping
`ext DcCall(sid, n, ...)` ⇄ `DC::Call` / `DC::Call_V2`. Verify against a known feature call.
A third ext route exists: the **typed binding** at `rec+0x10` (ext `skills.c` / `afk.c` — "its
rec+8 is a descriptor"), called with arguments in RCX/RDX/R8/R9 and the result in RAX; that is
`DC::CallTyped<"name">` (added with the AFK port, `menu2-player-menu-active?`). Document all three.

**Gate.** Port one native-heavy call path (e.g. the `notice` 0xBA poll) using only mod wrappers; no
new lookup code.

---

## 2. Net event channel — exists, reserve slots

**Ext mechanism.** The ext sends peer messages through the client's own event channel: it hooks
winmm's `SendEvent` (`FUN_18005dc60` — contract entry `kCode5DC60`, type/payload/size/dst) and calls
the trampoline with its own opcodes: `MSG_FINISHER_END 0x41` … `MSG_FLASH 0xAB` … `MSG_SHOTRAY 0xAF`
(≈60 opcodes). Types `0xB`/`0xC` are the base respawn request/grant.

**Mod today** (`src/net/protocol/net-protocol.hpp`, `src/net/online/net-session.hpp`,
`src/net/game/net-replication.cpp`): `NetEventType` (0..18) + `SendEventRaw(uint8_t type, …)` +
`SendEvent<T>` + a dispatch `switch` at `net-replication.cpp:~1568`. **This is the same channel** —
the source build calls `SendEvent` directly, so no send hook is needed.

**Gap.** Two things:
1. The ext's opcodes live *outside* `NetEventType`'s numbering. A port must add named `NEVT_*`
   entries with the **exact byte values** the ext uses (`NEVT_* = 0x90`, …), or a ported client and
   an un-ported (ext) client cannot talk during the coexistence window.
2. A registry so two features never claim the same opcode (the ext's `hookreg.h` does this for hooks;
   the event channel needs the equivalent).

**Deliverable.**
- Extend `NetEventType` with the ext opcodes, explicit values, one comment per slot citing its ext
  `MSG_*`. Keep the existing 0..18 low range untouched.
- A `NEVT_*` payload struct + `static_assert(sizeof==)` + a `case` in the dispatch switch, per ported
  feature (added *with the feature*, not all up front — but the slot numbers are reserved now).
- A table in `PORTING.md`: ext `MSG_*` ⇄ `NEVT_*` ⇄ owning feature module. Treat it as a fixed wire
  contract while both builds ship.

**Gate.** Port one single-message net feature (`scopeglint` `MSG_GLINT 0x91` or `glass` `MSG_GLASS
0x7A`) and A/B a ported client against an ext client: the message is produced and consumed on both.

---

## 3. Widgets / HUD — partial, needs the tree + transform layer

**Ext mechanism.** Spawns into `canvas.hudRoot` / page widgets and *walks and transforms* them:
`GUI2_SYSTEM 0x1270A80`, `GUI2_FIND 0x1E8F190` (path → widget), `GUI2_CHILD 0x1E8F5E0`, plus DC
natives `gui2-get-widget-transform` (`0xe2883ec3b0795b47`), `gui2-rxfm-set-pivot-as`
(`0xe85e01d3bbca99b9`), `gui2-rxfm-set-scale-ps` (`0xf5835b65cca9a9ce`). Widget definitions come
from `tools/features/*_widgets.py` (denynote, killfeed, clan13, gearup13, …).

**Mod today** (`src/engine/script.hpp` `GUI2`/`MENU2`; `src/net/ui/net-gui2.*`): `SPAWN_WIDGET`,
`SET_WIDGET_TEXT`, `SET_WIDGET_IMAGE`, `SET_WIDGET_VISIBLE`, `WIDGET_EXISTS`; `MENU2::PUSH_PAGE`,
`GET_PATH`, `GET_TOP_PAGE_IN_STACK`, `GET_PATH_IN_CURRENT_PAGE`, `IS_PAGE_ON_STACK`. The pak is built
by the gui2 CLI from `widgets/*.widget.json` (`CMakeLists.txt:157-171`).

**Gap.**
- No widget-tree navigation (`FIND`/`CHILD`/sibling) and no transform setters (pivot/scale/position)
  — needed by every "put this element at that anchor" HUD feature (`radar_box`, `killfeed`,
  `clanperk`, `rankbadge`, `interrogation_hud`, …).
- No reusable per-frame draw helper.
- **`mod/widgets/` and `mod/psarc/` do not exist in this tree**, yet `CMakeLists.txt` `FATAL_ERROR`s
  when they are empty — the widget/asset side of the build is currently unsatisfiable here. Porting
  any widget feature needs those sources.

**Deliverable.**
- Extend the `GUI2` namespace: `FIND(path)`, `CHILD(path)`, `GET_TRANSFORM`, `SET_PIVOT`, `SET_SCALE`
  (+ position/alpha as the ext uses them), and a `hudRoot` spawn helper.
- A widget-draw helper (one place that spawns-or-finds, sets text/image/visibility, transforms).
- Bring the widget definitions the port needs into `mod/widgets/` (translated from the ext's
  `*_widgets.py` generators), so the gui2 pak builds.

**Gate.** Port one pure-widget feature end-to-end (`notify` or `denynote`) using only mod widget APIs
and a committed `mod/widgets/*.widget.json`.

---

## 4. PCM audio mixer — partial (`engine/pcm.*`)

**Ext mechanism.** A `waveOut`-based raw-PCM player (`loadseq.c`, `t2r-ext.c`) with per-cue gain,
fade, loop and (for some) positional playback. ~16 features depend on it (`alert_sting`, `bomb_roll`,
`death_theme`, `downed_music`, `downed_vox`, `exec_beg`, `exec_music`, `explus`, `footsteps`,
`intfoley`, `pmsfx`, `pullout_sting`, `shiv_sting`, `armor_audio`, `match_horn`, `sp_parity`).

**Mod today.** Only engine sounds keyed by SID: `AUDIO::PLAY_SOUND`, `PLAY_SOUND_AT_POSITION`,
`PLAY_VOICE_LINE`, `AUDIO_GAMEPLAY_EVENT`, `PLAY_MUSIC_PLAYLIST`, `STOP_MUSIC_PLAYLIST`. There is
**no** raw-PCM path.

**Ported.** `src/engine/pcm.hpp/.cpp` reproduces the ext's mixer *behaviour*: one persistent
`waveOut` stream for the process lifetime (real winmm loaded from System32 by full path — this
module is itself the winmm proxy), a software mixer thread resampling voices to 48 kHz stereo into
6 x 20 ms event-callback buffers, 32 voices, a 2 ms fade in / out per voice, the soft limiter (knee
0.8, tanh to 1.0) and reopen-after-repeated-write-failure. Voices are typed handles (`Voice { slot,
src }`), so a slot reused by another sound makes `Live()` false and `Cut()` a no-op. The gain model
(`SfxUserVolume` / `MusicUserVolume`, ext `SfxOptionRaw` / `GeneralVolume` / `exmus_user_volume`)
reads the game's own sliders times the general volume, degrading to 1.0 when the (still INFERRED)
engine offsets cannot be read. The first two features sit on it: `net/audio/shivsting.*` and
`net/audio/alertsting.*`. The blade SFX (`net/audio/bladesfx.*`) sits on the 3D voice path.

**Dropped on purpose.** The ext's page-fault / prefetch / warm-worker, lock-timing instrumentation,
mix-dump file and MMCSS workarounds (dev-only perf, not behaviour).

**3D voices (ported).** The ext's dynamic voice fields (`dyn` / `ent` / `wp` / `rnear` / `rfar` + the
per-frame gain ramp) are ported: a dynamic voice follows its body (`ent`, or a world point) and is
panned / attenuated against the listener every frame by `engine::pcm::VoicesUpdate()` (called from
`boot::FrameTick`). The listener is the camera-through-reticle locator via the
`camera-get-dir-through-reticle` native, with the own-body-at-head-height fallback; the ext's
render-camera (icam) hook is deliberately not ported (documented in `pcm.cpp`). First consumer: the
blade SFX (`net/audio/bladesfx.*`), played 3D on the blade's body.

**Combat music tier state (ported).** `net/audio/music-tier.*` ports the *state* other features
gate on: `MusicEvent` (heat + the event / contact / awareness clocks), the heat decay, the local
producers we can observe (fire / melee / paired-move rising edges, health drops, the relayed hit),
the No Return fight clock + minimum dwell (0.9 s / 6.5 s), the old 3-step heat ladder behind
`t2r-music-nr-rules-old.txt`, and `g_msDead` from local HP. `alertsting` now refreshes its calm
clock on tier 0 and gates on dead / downed, so the "shot at out of the quiet" path can arm; the
damage event calls `Combat()` before `NoteEvent("we took a hit")`, matching the ext's hook order.
The *music playback* (risers, loops, beds, themes / sets / v5 rotation, grid-quantised cut timing,
stinger layer) is deliberately not ported: it needs the private PCM blobs and the generated
`music_families.h` / `music_pools.h` tables. The enemy-aware floor (sight / close / spot calls /
enemy shots / stuns), the lives-tiers 4 / 5 and the final-30 s floor stay honest TODOs.

**Gap.** Loop points and the crossfade used by `downed_music`, and the data wiring (item 7): the PCM
blobs are private generators, so `net/audio/*` compiles with the `T2R_DATA_*` gates off and
`Start()` / `Play()` are no-ops until the headers exist. The remaining sting / blade *triggers*
(the xcam hook and the melee-label hooks) are also unported, so the wired cues cannot all fire on
their own yet.

**Deliverable.** `src/engine/pcm.hpp/.cpp` — done. Source the buffers from generated PCM headers
(item 7). `mod/scripts/make-xvag.py` already exists and is the model for the generator wiring.

**Gate.** Port one PCM feature (`shiv_sting` or `alert_sting`) and confirm the cue plays, fades and
loops identically to the ext's.

---

## 5. Camera control — partial

**Ext mechanism.** Switches the **final render camera** (not just a DC overlay), plays per-frame
camera *tracks* generated from SP clips (`dcam_tracks.h`, `xcam_tracks*.h`, `pullout_cam_tracks.h`,
`pinesc_cam_tracks.h`), and keeps continuity (no one-frame cuts — `melee_camcont`). ~15 modules touch
the camera.

**Mod today** (`src/engine/script.hpp` `CAMERA`): `GET_CAMERA_LOCATOR`, `SET_OVERLAY`,
`OVERRIDE_FOV`, `SET_CAMERA_POSITION`; used in `net-replication.cpp` (spectate) and `net-gui2.cpp`.

**Gap.** The render-camera path the ext needs (writing the final camera, not the script camera) and a
track evaluator that can play a generated track and blend back. FOV continuity.

**Deliverable.** Extend `CAMERA` with the render-camera hook (safetyhook mid-hook on the camera
update) + a `CameraTrack` evaluator that consumes generated tracks. Keep the DC wrappers as-is.

**Gate.** Port one camera feature (`meleefov` — FOV only — or `downcam`) and match the ext's framing.

---

## 6. Pad injection — ported (call sites resolved by rel32 scan)

**Ext mechanism** (`padinject.c`). The engine's gameplay input is the `Pad`→`Joypad` pipeline, not
XInput. The ext re-points the 5 `Joypad::Update` call sites (+2 reader sites) to thunks that OR an
injected button mask into `Pad+0x528` and set the injected stick floats, then call the real
`Joypad::Update` — giving a genuine press edge with timestamps. Gated by
`t2r-no-pad-inject.txt`. Needed by `bot`, `melee_dodge`, QTE, and the test harnesses.

**Mod today.** `engine/pad.*` ports the **whole** virtual pad. `Joypad::Update` and the command
reader are resolved by prologue pattern (`Patterns::Joypad_Update` / `Joypad_CommandReader`), with
the ext's pinned RVAs (`0x1593350` / `0x158EB70`) preferred when their prologue still matches.
Because the ext's call-site *byte* patterns and `re/pad-inject-1026.md` are not in the repo, the
call sites are found dynamically: every executable image page is scanned for `E8` / `E9` rel32
references targeting each function, and a **safetyhook mid-hook** is installed at each site (plus
a return-probe hook after reader calls to read the return value in RAX). This replaces the ext's
hardcoded site RVAs and near-image RWX thunks with the mod's hooking mechanism. The hook keeps the
ext's exact delivery semantics: a press is applied for at least 2 `Joypad::Update` calls, min
20 ms, hard expiry +600 ms, and a bit already held is cleared on the first frame to force a fresh
edge. It learns each command's input bit from the engine's command table (press kind 8, alternate
0x20, held 2/4; lowest set bit of the first match), exposes `Press` / `BotPress` / `Stick` /
`SetBlock` / `MeleeEdge` / dodge / raw-stick helpers, and tracks `Applied()` / `StickApplied()`
counters for AFK. Activity stamping still samples the real Pad **before** injection. The one
version-guarded datum is the debug "Enable Joypad w/o Focus" byte (`RVA 0x8E03F65`), which cannot
be pattern-matched; on a mismatch the unfocused-window input helper is simply disabled.
`t2r-no-pad-inject.txt` turns the whole module off; `t2r-dodge-pad-ps53-old.txt` selects the old
dodge-input rule.

**AFK watchdog (ported).** `net/match/afk.*` mirrors every gate in ext `afk.c`:
live match / match over, downed / dead (`player-status` stubs), the 15-frame
`character-is-dead?` poll, and the three in-play menus — backpack, GEAR UP page
(`gui2::GearUpOpen()` checks the `to1/gearup13` top page only, so gearup13 itself stays
unported) and the player menu. `Tick()` / `Draw()` are wired in the frame and HUD paths and
`Reset()` runs on session reset. The 0xE3 trailer writer (`ReqTail`) reaches the daemon
through the JIP report below, and the test-server no-AFK flag (`g_srvTestFlags & 0x40`)
arrives through `net::daemon::TestFlags()`. The virtual pad's injection counters
(`engine::pad::Applied` / `StickApplied`) now count a playing dev bot as active.

**JIP 0xE3 clock report (ported: wire / clock / server settings only).**
`net/online/jip-report.{hpp,cpp}` reimplements the ext's `FeatureJoinInProgress` request
side: 2 s host / 3 s client cadence, clock-valid / match-over flags, viewer-relative team
and lives, the AFK trailer, one bounded TCP request per cadence via `net::daemon::Rpc`,
the generation check that drops stale replies, "enabled = 0" stop, the 3-miss / 20 s
backoff, and the 0xE5 reply's test-server / wave-server settings. It runs immediately
after `net::afk::Tick()` and resets with the session; `t2r-no-jip.txt` turns it off.
Honest gates: the 0xE5 roster merge into the lobby object, the in-match team-swap block
(`0xA7`), the late-join query (flags `0x04`), ghost peer reaping and requeue (`0x20`) are
still owned by the ext. Consequently the mod does not send flags `0x10` / `0x20`.

**Gap.** No binary / RE notes are available locally to verify the call-site scan or the pinned
RVA prologues against the actual `tlou-ii.exe`; this needs a live build to confirm. The dev-bot
and melee-pace / QTE consumers that *use* the virtual pad (`bot`, `melee_dodge`, the test
harnesses) are not ported — the pad API they would call is present and ready. JIP's roster /
team-swap / late-join path is still in the ext.

**Deliverable (done).** `src/engine/pad.hpp/.cpp` exposes the full virtual pad; call sites are
resolved by rel32 scan and hooked with safetyhook mid-hooks, not the ext's hardcoded RVAs / thunks.

**Gate (open).** A dev-injected press starts a real melee swing (the ext's acceptance test:
"engine reads 27") — needs the bot / melee-pace consumer, which remains unported.

---

## 7. Generated-data pipeline — partial

**Ext mechanism.** `tools/ext/GENERATED.md`: 78 game-derived headers (audio PCM, animation carriers,
camera tracks, move tables, voice ids). Most generators are **private**; a handful are public
(`vox_lines.py`, `gen_vox_words.py`, `scar_draw_gen.py`). The ext cannot build without them.

**Mod today.** A working codegen convention already exists: `mod/cmake/gen-psarc-manifest.py`,
`gen-winmm-forwarders.py` wired via `add_custom_command` (`CMakeLists.txt:148-194`), plus
`mod/scripts/` (`make-xvag.py`, `add-text2-ciphers.py`, `ida-dump-func.py`, `patch-menu-music.py`).

**Gap.** The ext's generators are not wired into the mod build; and private generators block some
features outright.

**Deliverable.**
- A `mod/cmake/gen-*.py` convention + `add_custom_command` template for a data header, emitting into
  `${CMAKE_BINARY_DIR}/generated` (already on the include path).
- A **stub/guard strategy**: each data-driven port compiles without its data (`#ifdef T2R_DATA_<x>`),
  so features can land in order and the data follows when its generator is available.
- A row in `FEATURES.md`'s data table → "generator available (path)" / "private → blocked", so Phase 4
  ordering avoids blocked features.

**Gate.** One generated header (e.g. a PCM one via `make-xvag.py`) is produced by the build and
consumed by the Phase-1 PCM port.

---

## 8. Config, toggles, logging — cross-cutting

- **Toggles.** 1,636 distinct `t2r-*.txt` strings across 124 modules. Policy for Phase 1: classify
  each as (a) *release behaviour* → reproduced unconditionally, (b) *dev/debug* → dropped, (c)
  *tuning* → a mod config value. Do **not** port the file-probe mechanism. Mod already has
  compile-time `src/loader/config.hpp`; add a small runtime config surface only if a tuning knob needs
  to survive to release.
- **Logging.** Unify on `TO1_LOG` (`src/log.hpp`, compiled out in `TO1_RELEASE`). Each ported feature
  drops its ext `xlog` / `t2r-ext-boot.txt` trail and uses `TO1_LOG` with the same prefix (so the A/B
  grep still works).
- **Hooks.** All new hooks go through safetyhook; the ext's `hookreg.h` overlap detection becomes the
  build-time/CI check (`tools/ext/tests/test_hook_overlap.py` is the reference).

**Deliverable.** A short "porting conventions" section in `PORTING.md` covering the toggle policy,
the `TO1_LOG` prefix convention, and the safetyhook rule.

---

## 9. Engine RVA calls and hooks — partial

**Ext mechanism.** Beyond the DC natives, the ext calls the shipped exe directly at pinned RVAs
(`*(exe + rva)(…)`) and, where it must change an engine decision, overwrites a code site with a
hand-built RWX trampoline (`bp_pick` over the 30-byte prop-pick prologue; `padinject.c` over the
`Joypad::Update` call sites). Everything is pinned to `tlou-ii.exe 1.6.10721`.

**Mod today.** `engine/transform.cpp` (`ProcessWorld`, RVA `0x133DB30`), `engine/pcm.cpp` (winmm
from System32 + the engine volume readers), `engine/backpack-prop.cpp` (the
`PlayerBackpackAnimCtrl` vtable / Enter-Leave RVAs), `engine/clipfx.cpp` (the `part-effect` anim
event fire path, RVA `0x1b04824`) and `engine/vfx.cpp` (particle spawn + compute-entry / retry)
already call pinned RVAs; the hook sites use
**safetyhook** (`net-patches.cpp`, `loader/boot.cpp`, `engine/pad.cpp`), each with a prologue check
that disables the hook when the bytes differ — i.e. on a game update.

**Ported with the backpack prop spawn.** The ext's `BpSpawnReal` runs the engine's own
`PlayerBackpackAnimCtrl::SpawnProp` (RVA `0xafd7f0`) on a *copy* of our controller (so `+0x118` is a
real `Player` and the vfuncs / parent link stay valid) with the puppet's locator; the ext answered
the art pick by trampolining the site at `0xafd9f6`. The mod keeps the RVA call but replaces the
trampoline with a **safetyhook mid-hook at the site's `cmp`** (RVA `0xafda06`, `kRvaPickCmp`): the
callback rewrites the `[rsp+0x198]` art slot, then the original `cmp` / `je` run unchanged
(0 → skip the spawn, non-zero → spawn). The 30-byte site prologue is verified before the hook is
installed. `BpSendMsg2` (`0x15734d0`) and the set-parent builder (`0x14cfc60`) are called by RVA,
both behind `engine::ptr::Readable` staleness guards.

**Ported with the weapon reparent / attach.** The ext reparents the slung weapons onto the bag at
the bag-off hand-off by hooking **12 call sites** (`bp_reparent_hook`, `fn_bp_reparent` at RVA
`0xafc740`) and **8 attach sites** (`bp_attach_hook`, `fn_bp_attach` at RVA `0x1cd2140`) with
hand-built trampolines. The mod instead safetyhook-inlines the **two engine function bodies**
themselves (the sites all call into them), so one hook covers every caller — a documented deviation.
A `g_attachBlend` gate selects the bag-off path; a `g_inAttach` re-entry guard passes the blend
path's own internal call to the instant attach through silently. `t2r-bp-weapons-body.txt` keeps the
weapons on the body (no reparent).

**Ported with the armour rig.** `engine/armor-rig.*` ports the skinned-armour tables + the typed
`look2` resolver (RVA `0x13578a0`, 15-byte prologue + the `look2` type sid) + the per-body apply
(`GvisSet` / `GvisRead` over the draw-control mesh instances). Its per-frame **driver**
(`feature_armor_visual`, the helmet / vest mask from the armour points) is not ported — no armour
feature exists in the mod — so the own-side rig stays unprobed and `backpack-prop`'s rig branch is
inert. The **ps57** put-away-refusal recovery (`CurrentWeapon` + the refusal-frame registry) is
ported too; its producer (`g_bp57RefuseF`, the unported holster fix) is honestly gated, so the
recovery is faithful but inert until that lands.

**Gap.** No pattern resolution (the RVAs are pinned); no shared helper for "call an exe RVA" (every
caller does its own `GetModuleHandleW` + cast). A game update silently disables the prologue-checked
hooks and mis-calls the unchecked RVA calls.

**Deliverable.** A small `engine/exe.hpp` (raw-call helper, distinct from `engine/exe_side.hpp`,
which is sidecar-file only): `exe::Base()`, `exe::Call<Ret>(rva, args…)` and a prologue-checked
`exe::HookMid(rva, bytes, fn)`. Migrate the raw call sites onto it.

**Gate.** The backpack prop spawn works on a ported client (the animated TAB prop appears) and the
ext's `bp_pick` trampoline can be dropped.

---

## Sequencing

Ordered by dependency, cheapest-unblock-first:

1. **1 + 2 + 8** — native invocation (verify), net slot registry, config/logging policy. Unblocks
   every net/UI/logic feature; almost no new code.
2. **3 + 7** — widget tree/transform + data pipeline + `mod/widgets/`. Unblocks the UI domain.
3. **4** — PCM mixer (+ its generated PCM). Unblocks the audio domain.
4. **5 + 6** — camera render path + pad injection. Only needed by melee/exec (P4), so they can land
   later without blocking P1–P3.

## Definition of done for Phase 1

The mod can host a port of any single feature, demonstrated by three **seam-proving ports** — one per
newly-touched mechanism, chosen to be small and self-contained:

| Proving feature | Exercises |
|---|---|
| `notify` (or `denynote`) | widget spawn + transform + a `mod/widgets/` def |
| `scopeglint` (or `glass`) | net slot reservation + exact-byte interop |
| `alert_sting` (or `shiv_sting`) | PCM mixer + generated PCM |

Each is ported, A/B'd against the ext build, then **disabled in the ext** via its existing toggle —
which is the Phase-2 cutover pattern in miniature.

## Deliverables (files)

| File | Change |
|---|---|
| `mod/src/engine/script.hpp` | `DC::Native` cached helper; extend `GUI2` (find/child/transform); camera track hook |
| `mod/src/engine/pcm.hpp` + `.cpp` | **new** — PCM mixer |
| `mod/src/engine/pad.hpp` + `.cpp` | **new** — pad injection (safetyhook) |
| `mod/src/net/protocol/net-protocol.hpp` | ext `NEVT_*` slot values + payload structs |
| `mod/src/net/game/net-replication.cpp` | dispatch `case`s for ported slots |
| `mod/widgets/*.widget.json` | **new** — widget defs translated from `tools/features/*_widgets.py` |
| `mod/cmake/gen-*.py` + `mod/CMakeLists.txt` | data-header codegen template + wiring |
| `mod/src/loader/config.hpp` (+ runtime config if needed) | toggle policy surface |
| `mod/src/engine/ptr.hpp` | `Readable` / `Writable` guarded pointer reads (ext `IsBadReadPtr` / raw deref) |
| `mod/src/engine/frame.hpp` + `.cpp` | QPC ms clock (`NowMs`, ext `BpMs`) alongside the 60 Hz frame counter |
| `mod/src/engine/object.hpp` + `.cpp` | **new** — entity / joint locators, draw-control mesh hide/show, set-locator (ext `XbxLoc` / `GvisDc` / `BpDc` / `PsyncBfFromLoc`) |
| `mod/src/engine/anim-prims.hpp` + `.cpp` | **new** — `AnimateFrom` / `AnimLen` (ext `animate_` recipe + `get-animation-duration`) |
| `mod/src/engine/backpack-prop.hpp` + `.cpp` | **new** — own-side backpack hand-off (ext `feature_backpack_prop`) |
| `mod/src/engine/armor-rig.hpp` + `.cpp` | **new** — skinned-armour tables + `look2` resolver + per-body apply (ext `ARMOR_RIG_*` / `ArmorRigApply`); the armour-visual driver is unported |
| `mod/src/net/match/backpack.hpp` + `.cpp` | **new** — TAB backpack wire + prop state machine + engine prop spawn (ext `backpack.c` + `bp_pick`) |
| `mod/src/engine/clipfx.hpp` + `.cpp` | **new** — engine `part-effect` anim-event fire path (ext `clipfx.c`) |
| `mod/src/engine/vfx.hpp` + `.cpp` | **new** — particle spawn + compute-entry / retry (ext `CfxSpawnRaw` / `ArmorFxSpawn` / `CfxRetryTick`) |
| `mod/src/gameplay/melee2013.hpp` + `.cpp` | **new** — 2013 melee move table (ext `MELEE2013` / `MxMove` / `MxByMove`) |
| `mod/src/gameplay/ctxdown.hpp` + `.cpp` + `ctxdown-rows/*.h` | **new** — contextual-down pair table (ext `ctxdown.c` `CD_PAIRS` + `mduel_*` / `pullout_rows.h`) |
| `mod/src/net/anim/paired.hpp` + `.cpp` | **new** — psync paired-move surface + abort hook (ext `paired.c` `g_ps` subset) |
| `mod/src/net/match/bloodbuild.hpp` + `.cpp` | **new** — per-life blood / dirt build-up (ext `bloodbuild.c`) |
| `mod/src/net/audio/bladesfx.hpp` + `.cpp` | **new** — TLOU2 blade cues (ext `blade_sfx.h`) |
| `mod/src/net/audio/music-tier.hpp` + `.cpp` | **new** — combat music tier STATE + `g_msDead` (ext `MusicEvent` / `FeatureCombatMusic`); playback stays in the ext |
| `mod/src/engine/exe.hpp` | **new** — raw exe-RVA call + prologue-checked safetyhook mid-hook helper (ext pinned-RVA calls / RWX trampolines) |
| `mod/PORTING.md` | mechanisms section: DC mapping, `MSG_*`⇄`NEVT_*` table, toggle/log/hook conventions |
| `mod/FEATURES.md` | data-table column: generator availability |

## Risks

- **No local build here.** Every gate above needs the user's Windows toolchain (clang-cl + LLVM +
  game install); this environment can write/refactor source and scripts but cannot run the A/B.
- **Wire compatibility.** Ported and ext clients share a lobby during coexistence; net opcodes must
  keep their exact byte values, and no port may reuse an ext opcode.
- **Private generators.** Features whose data generator is private cannot be fully ported until the
  generator is available; the stub strategy keeps them compiling, and `FEATURES.md` marks them blocked
  so Phase 4 ordering avoids them.
- **Pad injection is version-sensitive.** The ext's RVAs are pinned to `tlou-ii.exe 1.6.10721`; the
  source build must resolve the call sites by pattern, and re-verify on a game update.
- **Audio device contention.** A `waveOut` mixer inside winmm coexists with the engine's own audio; if
  it causes device conflicts, fall back to the engine's voice API.
- **`mod/widgets/` / `mod/psarc/` are absent here.** The widget/asset half of the mod build cannot
  currently run in this tree; Phase 1 must establish those sources before any UI feature can be built.
