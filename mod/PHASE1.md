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
| 4 | PCM audio mixer | `waveOut*` in `loadseq.c`, `t2r-ext.c` | **missing** — only engine `AUDIO::PLAY_SOUND` | whole mixer |
| 5 | Camera control | render-cam switch + `*_tracks.h` | **partial** — `CAMERA::SET_CAMERA_POSITION/OVERRIDE_FOV` | render-camera path + track evaluator |
| 6 | Pad injection | `padinject.c` (writes `Pad` before `Joypad::Update`) | **missing** — only reads `JOYPAD_COMMAND_ACTIVE` | whole injection path |
| 7 | Generated data pipeline | 78 headers (`tools/ext/GENERATED.md`) | **partial** — `mod/cmake/*.py`, `mod/scripts/make-xvag.py` | wire ext generators / stub strategy |
| 8 | Config, toggles, logging | 1,636 `t2r-*.txt` + `xlog` | `TO1_LOG` + compile-time `config.hpp` | toggle policy + runtime config |

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

## 4. PCM audio mixer — missing

**Ext mechanism.** A `waveOut`-based raw-PCM player (`loadseq.c`, `t2r-ext.c`) with per-cue gain,
fade, loop and (for some) positional playback. ~16 features depend on it (`alert_sting`, `bomb_roll`,
`death_theme`, `downed_music`, `downed_vox`, `exec_beg`, `exec_music`, `explus`, `footsteps`,
`intfoley`, `pmsfx`, `pullout_sting`, `shiv_sting`, `armor_audio`, `match_horn`, `sp_parity`).

**Mod today.** Only engine sounds keyed by SID: `AUDIO::PLAY_SOUND`, `PLAY_SOUND_AT_POSITION`,
`PLAY_VOICE_LINE`, `AUDIO_GAMEPLAY_EVENT`, `PLAY_MUSIC_PLAYLIST`, `STOP_MUSIC_PLAYLIST`. There is
**no** raw-PCM path.

**Gap.** The whole mixer. Design decision: `waveOut` (what the ext does — simple, but a second audio
device consumer inside winmm) **vs** the engine's own voice API (if a raw-buffer voice is reachable).
Reproduce the ext's *behaviour* (gain curve, fade in/out, loop points, crossfade for `downed_music`)
regardless of the backend.

**Deliverable.** `src/engine/pcm.hpp/.cpp`: load a PCM buffer, play with gain/fade/loop, stop,
per-cue handles, and a per-frame `Tick()` (called from the mod's frame hook). Source the buffers from
generated PCM headers (item 7). `mod/scripts/make-xvag.py` already exists and is the model for the
generator wiring.

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

## 6. Pad injection — missing

**Ext mechanism** (`padinject.c`). The engine's gameplay input is the `Pad`→`Joypad` pipeline, not
XInput. The ext re-points the 5 `Joypad::Update` call sites (+2 reader sites) to thunks that OR an
injected button mask into `Pad+0x528` and set the injected stick floats, then call the real
`Joypad::Update` — giving a genuine press edge with timestamps. Gated by
`t2r-no-pad-inject.txt`. Needed by `bot`, `melee_dodge`, QTE, and the test harnesses.

**Mod today.** Read-only: `DC::JOYPAD_COMMAND_ACTIVE`. No injection.

**Gap.** The whole injection path.

**Deliverable.** `src/engine/pad.hpp/.cpp`: inject button bits / stick axes for a bounded window,
installed with **safetyhook** (mid-hooks at the same call sites) instead of the ext's hand-built
thunks, resolved by **pattern scan** rather than the ext's hardcoded RVAs. One place, one registry
entry.

**Gate.** A dev-injected press starts a real melee swing (the ext's acceptance test:
"engine reads 27").

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
