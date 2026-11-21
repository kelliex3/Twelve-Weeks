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

**Combat music tier state + floors (ported).** `net/audio/music-tier.*` ports the *state* other
features gate on: `MusicEvent` (heat + the event / contact / awareness clocks), the heat decay, the
local producers we can observe (fire / melee / paired-move rising edges, health drops, the relayed
hit), the No Return fight clock + minimum dwell (0.9 s / 6.5 s), the old 3-step heat ladder behind
`t2r-music-nr-rules-old.txt`, and `g_msDead` from local HP. The **tier floors** are now native too:
enemy-aware heat with a 6 s hold, marked / seen near / far / close, close quarters, down /
execution-pending, low health in a fight, the final-30 s and final-10 s floors, lives tiers 4 / 5
and the "lives came back" exit. They read only mod-observable peer positions / team / mark / death
flags, team lives and the match clock. `alertsting` refreshes its calm clock on tier 0 and gates on
dead / downed, so the "shot at out of the quiet" path can arm; the damage event calls `Combat()`
before `NoteEvent("we took a hit")`, matching the ext's hook order. The *music playback* (risers,
loops, beds, themes / sets / v5 rotation, grid-quantised cut timing, stinger layer) is deliberately
not ported: it needs the private PCM blobs and the generated `music_families.h` / `music_pools.h`
tables. Honest gates: the camera-frustum / LOS test and the 5–8 m forward-cone sight check stay
gated (no render-camera path), so those two floor inputs are conservative until that mechanism lands.

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
live match / match over, downed / dead (`player-status` reads the dormant downed core), the
15-frame `character-is-dead?` poll, and the three in-play menus — backpack, GEAR UP page
(`gui2::GearUpOpen()` checks the `to1/gearup13` top page only, so gearup13 itself stays
unported) and the player menu. `Tick()` / `Draw()` are wired in the frame and HUD paths and
`Reset()` runs on session reset. The 0xE3 trailer writer (`ReqTail`) reaches the daemon
through the JIP report below, and the test-server no-AFK flag (`g_srvTestFlags & 0x40`)
arrives through `net::daemon::TestFlags()`. The virtual pad's injection counters
(`engine::pad::Applied` / `StickApplied`) now count a playing dev bot as active.

**JIP 0xE3 clock report + 0xE5 reply cores (ported, engine writes gated).**
`net/online/jip-report.{hpp,cpp}` reimplements the ext's `FeatureJoinInProgress` request
side: 2 s host / 3 s client cadence, clock-valid / match-over flags, viewer-relative team
and lives, the AFK trailer, one bounded TCP request per cadence via `net::daemon::Rpc`,
the generation check that drops stale replies, "enabled = 0" stop, the 3-miss / 20 s
backoff, and the 0xE5 reply's test-server / wave-server settings. It runs immediately
after `net::afk::Tick()` and resets with the session; `t2r-no-jip.txt` turns it off.
The reply side now ports three ext cores against a **native roster shadow** seeded from the
read-only `engine/lobby.*` view:
- **0xE5 roster merge** — prune slots the server no longer lists, replace known slots, append
  new ones, preserve our own entry and never let the daemon's `PLAYER n` fallback overwrite a
  real name; a team change for a matching name logs the "JOINED YOUR TEAM"/"MOVED TO THE ENEMY
  TEAM" banner. The listed slot mask + arrival stamp feed the reaper.
- **0xA7 in-match swap** — parse the trailing `[0xA7][k][k x (slot, team)]` block and apply only
  our own slot to the three places winmm keeps it (ctx+0x1c, roster +0x31, record +0x48), with
  the "YOU WERE MOVED TO THE OTHER TEAM" banner; ignored once the match is over.
- **Ghost peer reaping** — every in-match tick, a listed-but-silent, non-host peer with no fresh
  accepted-anim publish for `GHOST_SILENT_MS` (15 s) is a drop candidate; a listed mask older
  than 10 s is ignored, `t2r-ghost-peer-old.txt` opts out, and reaped slots are not re-flagged
  until they reappear.

All three are observation / decision only. Engine lobby writes, the winmm peer drop (through
`net::session::DropPeerForReaper`) and the 0x10 "send the swap block" flag are behind the
`kWriteLobby` / `kApplyGhostPeers` / `kRequestTeamSwap` constants (all false), so the ext stays
the behaviour owner and no toggle is flipped. The late-join query (`0x04`, kit carry) stays in
the ext. Requeue (`0x20`) is ported (`net/online/requeue.*`), so the report sends that flag
while `t2r-no-requeue.txt` is absent.

**Requeue and the direct melee path (ported with honest gates).** `net/online/requeue.*` owns
the 0xBC / 0xBD ticket poll, the post-match arm and settle, the `to1/find-match-menu` push and
the WAITING FOR PLAYERS... label. `net/game/net-direct.*` owns the helper loopback socket and
the direct message / ping / pong wire, with the 800 ms dedupe ring hooked into the existing
event channel at `SendEventRaw` and `HandleState`. Both use the same `t2r-*.txt` sidecars as
the ext. Requeue's post-match screen gate is native now: `net/ui/postmatch-screens.*` samples
the menu stack for the clan13x scoreboard, clan-postmatch counter, clan-event screen, GEAR UP
navigation and the `g_mmBack` refused-search latch, and requeue's `PostMatchScreensDone()` is a
single call into it. Honest gates: the ext's `g_pm.pending` / `g_ev.pending` are set when the page
is due (before it is pushed), which the mod cannot observe, and the `g_mmBack` page-pop is
gated because net-gui2 already pops the page it refuses. Net-direct's `Eligible` now mirrors the ext's
`NdTypeOk` exactly: the reserved melee opcodes (`0x6C`, `0x97..0x9A`, `0x9C`, plus PSYNC `0x4B`
and MV3 `0x9E` minus their PING / PONG) take the direct path too, so a ported client and an ext
client stay interoperable. This is a transport decision, independent of the (dormant) melee
bridge gate. Requeue's kick signal is now native (see below).

**Native kick timestamp / g_kickMs (ported).** `net/match/kick-state.*` owns the ext's
`g_kickMs`, `MsgIsKick` and `MsgIsMove`: the English / Polish kick prefixes and the team-move
prefix, and the tick stamp. The ext stamps it in `MsgHook` on the daemon's op 0x11 note; the
mod has no winmm message hook, so `notice::Apply` stamps it from the same kick text on the
kind-'p' notice line (daemon `kick_token` sends both). The stamp feeds the 120 s duplicate
check in `notice.c` (via `notice::Hooks::kickShownAt`, installed next to the friends13 hooks)
and `requeue::KickedSinceArm()` (the ext's `g_kickMs > g_rq.armed`). A removed AFK player needs
no separate path: `net/match/afk.*` already reports idle on the 0xE3 report and the daemon
returns the removal as this same kick text. Honest gates: the ext's winmm op 0x11 producer, the
kicknote MP-menu box (`KnSet`), and the team-move rejoin (`g_moveMs` / `move_rejoin_tick`) stay
in the ext; without the winmm hook the notice line is the only producer the mod can see. No
dedicated sidecar (the ext has none) -- the consumers keep `t2r-no-notice.txt` /
`t2r-no-requeue.txt`.

**Shared match-end latch (ported).** `net/match/match-liveness.*` is the native home of the
ext's `g_wasLive`: one once-per-frame read latched as `Live()` / `WasLive()` / `MatchOver()` /
`MatchEndHold()`. `engine/lobby.*` now supplies the exact ext signals — substate 6 at
`kNetCtx+0x2b80` and the `LobbyObj()+0x2ca4` end banner — readable through `engine::ptr`; the
mod's own match phase (in match / expired / scoreboard) is only the fallback when the engine
data is not up yet. The reads are observation-only (no lobby writes), so the ext still owns
the state. `player_status::MatchLive()` now reads this latch (instead of re-deriving the gate)
and AFK inherits the exact end banner. The tick runs first in `boot::FrameTick`, before
requeue (which arms on `MatchEndHold()`) and friends13 (which hides its panel / toasts during
the end sequence), so a single value is shared by all consumers.

**Post-match screen latches (ported, observation-only).** `net/ui/postmatch-screens.*` is the
native home of the five ext bools that hold requeue's settle after a match:

- **clan13x** — the post-match scoreboard page (`to1/clan13x`; ext `Cl13PostActive` / `g_c13x.post`).
- **clan-postmatch** — the supply-counter page (`to1/clan-postmatch`; ext `g_pm.pending`).
- **clan event** — the announcement / result screen (`to1/clan-event`; ext `g_ev.pending`).
- **GEAR UP nav** — any loadout / picker page (`gearup13`, `loadout-menu`, `picker-skill` /
  `-primary` / `-secondary` / `-purchase`), the observable equivalent of ext `g_loState != LO_NONE`.
- **g_mmBack** — a refused FIND MATCH holds the settle for three frames (ext `MmreqHook`), then
  the latch clears. The producer is wired in `net-gui2.cpp`'s party-gate branch; the ext's second
  producer (Steam helper not connected) has no mod equivalent.

Each page latch is sampled once per frame from `MENU2::IS_PAGE_ON_STACK`; `ScreensDone()` is the
single aggregate requeue consumes in `PostMatchScreensDone()`, replacing the old `return true`
stub. No page is pushed or popped: the ext still owns the page behaviour while it is the bridge
and `kPopOnMmBack == false` because net-gui2 already pops the page it refuses. Honest gates: the
ext sets `g_pm.pending` / `g_ev.pending` when the page is *due*, before it is pushed — the mod can
only observe the page itself, and requeue's 1.5 s settle absorbs that window. The CLAN page data,
the clan-event decision and GEAR UP's own `g_loState` transitions stay in the ext.

**In-game FRIENDS / PARTY panel (ported with honest gates).** `net/social/friends-state.*` is the
pure JSON/state core (ext `friends13.h`): bounded 0xD9 reply parser, rows, badge, presence and
actions. `net/social/friends13.*` is the feature: 0xD8/0xD9 async RPC + poll/action state machine,
panel page / rows / text entry, friend-request + party-invite cards (plain toasts when the widget
defs or `t2r-no-friend-toast.txt` opt out), P / pad button, lobby decorations, notice-c toast
hooks, and the party ready / follow rule + search gate. The gate is wired into `net-gui2.cpp`
just before `FindMatch` (a member's press toggles ready instead of searching) and into requeue
(`IsMember()` / `NoteAutoPush()`). The 2013 rank badge is now native for these rows: `friends-state.*`
holds the pure 0xBF table (name key / reply parser / lookup / rank13 helpers), `rank-badge.*` owns
the async 0xBF poll and `RankUi` widget drawing (2.5 s after a miss, 12 s otherwise, only while a
screen drew a badge in the last 15 s, 20 s retry after two unanswered tries) and `SetRow` reserves
the 120-unit badge width; MATCH FOUND team tabs / names use `engine::lobby::RosterSorted` /
`RosterName`. Click-to-nav is native as well: the ext's `FrScan` canvas boxes hit-test the rows and
footer prompts through the native cursor canvas; a row click only moves the focus (the engine's
mouse-sourced menu-select is dropped for 8 frames, as in the ext), and the hovered row gets the
`Fill`/`SetRow` highlight. Honest gates: the exact raw `kPartyStatus` byte, the searchers' table
(`g_srchTab` / `g_srchName` -- no mod producer for the daemon's relative [T1]/[T2] markers), lobby
object writes, exact `menu-*` pad bindings and the card's per-widget keyframes stay in the ext;
widget writes are inert when the defs are absent.

**Gap.** No binary / RE notes are available locally to verify the call-site scan or the pinned
RVA prologues against the actual `tlou-ii.exe`; this needs a live build to confirm. The dev-bot
and melee-pace / QTE consumers that *use* the virtual pad (`bot`, `melee_dodge`, the test
harnesses) are not ported — the pad API they would call is present and ready. JIP's roster /
team-swap / ghost-reap **cores** are native now but their engine writes / peer drops are gated
off; the late-join query (`0x04`) is still in the ext. The pinned lobby / peer-record RVA calls
need a live exe to verify.

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

**Ported with the own-side poll.** The SP backpack stop is native: while our menu is open in a
live match, up, and the bag is active (or still inside the 1.2 s stow / intro window, or
`t2r-bp-hold-old.txt` is present), `player-disable-move-stick` / `-jump` / `-roll` run every
frame; closing the menu hands the stick straight back. `t2r-bp-menu-move.txt` restores the old
MP behaviour. The ext's speed log is replaced by the menu state (the mod has no momentum reader)
and the downed gate reads the dormant downed core like every other consumer.

**Gap.** No pattern resolution (the RVAs are pinned); no shared helper for "call an exe RVA" (every
caller does its own `GetModuleHandleW` + cast). A game update silently disables the prologue-checked
hooks and mis-calls the unchecked RVA calls.

**Deliverable.** A small `engine/exe.hpp` (raw-call helper, distinct from `engine/exe_side.hpp`,
which is sidecar-file only): `exe::Base()`, `exe::Call<Ret>(rva, args…)` and a prologue-checked
`exe::HookMid(rva, bytes, fn)`. Migrate the raw call sites onto it.

**Gate.** The backpack prop spawn works on a ported client (the animated TAB prop appears) and the
ext's `bp_pick` trampoline can be dropped.

---

## 10. Melee reliability / state-machine cores (ported, gated)

Beyond the enabling mechanisms above, this batch ports the **logic cores** of the melee
reliability / state-machine features. Each is a pure module behind a `Sink` / callback bridge, so
it is inert until a caller supplies the engine / transport producers. That is deliberate: the ext
stays the bridge and no cutover toggle is flipped. The exact ext wire IDs are reserved in
`net-protocol.hpp` so a ported client can coexist on the wire (`NEVT_HIT_REL 0x6C`,
`NEVT_DODGE_* 0x97..0x99`, `NEVT_HITREACT2 0x9A`, `NEVT_MEX 0x9C`, `NEVT_MV3 0x9E`,
`NEVT_PSYNC 0x4B`, `NEVT_ARMOR_HIT 0x4A`).

- **Reliable one-shot hits** — `net/match/hit-rel.*` (ext `t2r-ext.c` hitrel section). Per-destination
  sequence, capability latch, `{0,60,150,300,450,600}` ms retransmits, ACKs, a 64-entry aged dedupe
  ring, slot cleanup and stats. Wire `[u8 op][u16 seq][u8 victim][payload]`, ops 0 hello / 1 hit /
  2 armour / 3 molotov / 4 ACK. Gate: transport send/recv and the type-3 / armour-hit producers stay
  in the ext until the relay bridge is ported.
- **Melee dodge** — `gameplay/melee-dodge.*`. Streak windows `480/260/180/130` ms, `380` ms reaction,
  `1000` ms hard cap, `260` ms i-frames, two charges with `2500/1500` ms cooldowns, swing tracking
  and press / dodged verdicts. Gate: pad injection is available (`engine/pad.*`) but the swing /
  contact / animation / prompt producers and the relay wire are not wired to it yet.
- **Hit reactions** — `gameplay/melee-hit-react.*`. `MHR_CUT_MS {360,440,520}` strength rule, the
  `MSG_HITREACT2` decode / flags / validation `[150,600]`, victim graze / replace / cut, attacker
  lock cap `480` ms + `900` ms confirm, and the 50 ms predicted puppet reaction. Gate: the SP clip
  table (`melee_hitreact_rm.h`, private), body pin / animation, pain VO, punch / blade FX and the
  busy / duel / downed gates stay behind `HitReactSink.PickClip` / effects.
- **Melee exchange** — `gameplay/melee-exchange.*`. Tune `{440,520,60}`, Lat / Vis / Tc math,
  `MSG_MEX` START / ACK / RES / END decode + build, role / beat state machine, shared-clock windows,
  verdicts, contact hold, combo / cooldown bookkeeping. Gate: press injection, the engine swing,
  contact replay, dcounter, seq / qte and the camera stay behind `MexSink`.
- **Melee v3** — `gameplay/mv3/`. The shared contract (`types.hpp`), the generated move table +
  deterministic integer sim (`sim.cpp`), and the engine-free bubble / rollback / wire core
  (`net-core.hpp`, header-only via `Mv3nIo`). Ported verbatim with the ext's pinned names because
  they are the shared spec / generated-data contract; only the namespace changed. Gate: pads,
  animation / root motion, FX, camera and the engine `Mv3nIo` bridge are not supplied, so the sim
  and netcore compile but never run in the mod yet. Asset / move-table parity cannot be verified
  without the game binary.

**Integration seams (landed, dormant).** `net/match/melee-bridge.*` is the single place that
installs one *inert* sink per core and fans out the frame tick (`boot::FrameTick`), the reserved
`NEVT_*` receive cases (`net-replication::HandleNetEvent`) and the session reset
(`net-replication::ResetRuntime`). The sinks supply the real clock / local slot / match-liveness
so the cores' bookkeeping stays consistent, but every engine / transport effect is a no-op and the
clip-table / RTT / peer-state gates return conservative defaults. The master gate
`melee_bridge::kEnabled` is **false**, so all four bridge functions no-op in a shipping build and
the ext remains the behaviour owner — no ext cutover, no `t2r-*.txt` toggle flipped. Send-side
interception (`HrSendHit` / `SendArmor` / `NoteOriginal`) and the MV3 `Mv3nIo` / PSYNC placement
bridges are separate gates not installed here. The remaining path to cutover is: port each real
producer, enable that feature in the bridge, then disable the matching ext module via its toggle.

---

## 11. Downed state core (ported, gated)

`net/match/downed-state.{hpp,cpp}` ports the core 2013 downed behaviour from `t2r-ext.c`
(`feature_downed`, the `DownedSet` / `RevivedRecv` / `RvSet` / `RvProg` helpers and the
`MSG_DOWNED` / `MSG_REVIVE` peer handlers). It owns only what can be reasoned about without
the engine:

- **State and wire** — down / dying / up, the send epoch (`++` on every transition), the downer
  slot (0xFE = revived, not a death), and the exact ext wire layouts: `MSG_DOWNED` 6 bytes
  `[state][downer][knockDir+1][supine/dieDir+1][bleed+0.5][epoch]` and `MSG_REVIVE` 1 byte
  `[who]`, reserved as `NEVT_DOWNED 0x43` / `NEVT_REVIVE 0x44`.
- **Pool and hit rules** — 50 hp downed pool (Fortitude II bonus via `DownHpMul`), 1 s down
  protection that ignores ordinary damage but never the 1e6 execution marker, and hits while
  downed shortening the remaining bleed-out by `damage / pool x base seconds` (floored at
  0.1 s and never landing sooner than 1 s after the hit).
- **Bleed-out clock** — real milliseconds (not frames), mode base 15 s / 7 s, the 2013
  per-revive cut (`1 - min(revives x .25, .5)`) or the legacy per-down table, multiplier from
  Fortitude / Heavy Hands, pause accumulation while a teammate revives, 1 s resends, and the
  3 x 300 ms repeats of the single-shot "up" message. `BleedFraction()` is the same clock the
  ext's HUD bar and downed music read.
- **Revive aggregation** — every attached `[target][reviver]` hold contributes
  `elapsed x rate / time`; rates 1 / 0.6 / 0.4 (configurable for `t2r-revive-corate.txt`),
  0.4 for the fourth and later, first-attached rank books the completion, lost "stop" expires
  after `2 x (secs + 4) s`, and a local reviver times out at `secs + 3 s`.
- **Peer tracking** — epoch-based reorder guard (10 s window), resend suppression, stale
  "downed" after our own `MSG_REVIVE`, and per-slot bleed / fall direction / supine / death
  direction for the (gated) visual consumers.

**Integration seam (landed, dormant).** `net/match/downed-bridge.*` installs one inert sink and
fans out the frame tick (`boot::FrameTick`), the two reserved `NEVT_*` receive cases
(`net-replication::HandleNetEvent`) and the session reset (`net-replication::ResetRuntime`). Its
clock / frame counter / local slot / match liveness are real; wire send, health read, skill
multipliers and every engine / award / camera / animation effect are no-ops or conservative
defaults. The master gate `downed_bridge::kEnabled` is **false**, so the ext remains the
behaviour owner and no ext cutover is flipped. The remaining path to cutover: port the hit
reception / UC4 reach producers, puppet handling, HUD bar and the send trampoline, enable the
bridge per producer, then disable the matching ext path via its toggle. The PCM audio modules
`downed_music.c` / `downed_vox.c` are separate inventory rows and stay unported.
`player_status::LocalDowned` / `PuppetDowned` now read this core instead of hard-false stubs,
so every existing consumer inherits a correct gate as soon as the bridge is enabled.

**Dying distinction (ported, producer gated).** The ext's other down-state latches are carried
in the same core: `g_downDying` (a lethal hit / bleed-out while down, set before the wire goes
to state 0) and `g_uc4DyingUntil` (the UC4 death-clip frame window). `SetLocalDying()` /
`SetDyingUntilFrame()` feed them, a fresh down clears them and a revive ends them, and
`LocalState()` / `PuppetDownState()` answer the exact `bloodbuild.c` distinction: 0 up, 1 down
(crawling), 2 dying. `bloodbuild.cpp` now reads those accessors instead of hard-coding `1` for
a downed body. No death-clip / lethal-hit producer is cut over yet, so the local latch stays
clear and the answer is 1 while down — the wire already carries 2 for a peer whenever the
downed bridge feeds `NEVT_DOWNED`.

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
| `mod/src/engine/lobby.hpp` + `.cpp` | **new** — read-only engine net lobby (ext `MatchSubstate` / `LobbyObj` / roster + peer-record + `TeamOf`): substate 6, `+0x2ca4` end banner, clock, roster helpers; `RosterSorted` (ext `LobbyRowsSorted`: our team blue first, enemy red) + `RosterName` for MATCH FOUND friends rows; no writes |
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
| `mod/src/net/audio/music-tier.hpp` + `.cpp` | **new** — combat music tier STATE + floors + `g_msDead` (ext `MusicEvent` / `FeatureCombatMusic`); playback stays in the ext |
| `mod/src/net/match/match-liveness.hpp` + `.cpp` | **new** — shared once-per-frame match-end latch (ext `g_wasLive`): engine substate 6 / end banner via `engine/lobby.*` with a mod-phase fallback; `Live()` / `WasLive()` / `MatchOver()` / `MatchEndHold()` consumed by requeue, friends13, `player_status` + AFK |
| `mod/src/net/match/kick-state.hpp` + `.cpp` | **new** — shared kick timestamp / text tests (ext `g_kickMs` / `MsgIsKick` / `MsgIsMove`), fed by the notice kind-'p' line, consumed by requeue + notice |
| `mod/src/net/ui/postmatch-screens.hpp` + `.cpp` | **new** — post-match / menu latches (ext clan13x `Cl13PostActive`, `g_pm.pending`, `g_ev.pending`, GEAR UP `g_loState`, `g_mmBack`): sampled once/frame from the menu stack; `ScreensDone()` is requeue's single gate; `g_mmBack` pop seam gated (`kPopOnMmBack == false`) |
| `mod/src/net/match/melee-bridge.hpp` + `.cpp` | **new** — gated integration seams: inert sinks + boot tick / net-event dispatch / session reset fan-out; master gate `kEnabled == false` |
| `mod/src/net/match/downed-state.hpp` + `.cpp` | **new** — pure 2013 downed core (ext `feature_downed` / `DownedSet` / `RvProg` / `MSG_DOWNED` / `MSG_REVIVE`): state + epoch, the dying latches (`g_downDying` / `g_uc4DyingUntil`) + 0/1/2 `LocalState()` / `PuppetDownState()`, pool / protection / bleed-out clock, multi-reviver aggregate, peer reorder guard, exact wire layouts, behind `DownedSink` |
| `mod/src/net/online/jip-report.hpp` + `.cpp` | extend — 0xE5 roster-shadow merge + 0xA7 own-team swap parse + ghost-peer reaper cores (observation-only; engine writes / peer drops / 0x10 gated false) |
| `mod/src/net/game/player-status.hpp` + `.cpp` | extend — `MatchLive()` reads the shared engine liveness latch; `LocalDownState()` / `PuppetDownState()` expose the 0/1/2 dying distinction to `bloodbuild.cpp` |
| `mod/src/net/match/downed-bridge.hpp` + `.cpp` | **new** — gated integration seam for the downed core: inert sink + boot tick / net-event dispatch / session reset fan-out; master gate `kEnabled == false` |
| `mod/src/net/match/hit-rel.hpp` + `.cpp` | **new** — reliable one-shot hit core (ext hitrel section): seq / retransmits / ACK / dedupe ring, behind `HitRelSink` |
| `mod/src/gameplay/melee-dodge.hpp` + `.cpp` | **new** — pure dodge timing / state core (ext `melee_dodge.c`), behind `DodgeSink` |
| `mod/src/gameplay/melee-hit-react.hpp` + `.cpp` | **new** — pure hit-reaction / attacker-lock / prediction core (ext `melee_hitreact.c`), behind `HitReactSink`; clip table gated |
| `mod/src/gameplay/melee-exchange.hpp` + `.cpp` | **new** — pure exchange state machine / wire core (ext `melee_exchange.c`), behind `MexSink` |
| `mod/src/gameplay/mv3/{types.hpp,sim.cpp,net-core.hpp,net-core.cpp}` | **new** — melee-v3 contract + generated table + deterministic sim + engine-free rollback / wire core (ext `mv3.h` / `mv3_moves.h` / `mv3_sim.c` / `mv3_net_core.h`); names pinned to the shared contract |
| `mod/src/net/social/friends-state.hpp` + `.cpp` | **new** — pure 0xD9 JSON state / rows / actions core (ext `friends13.h`) + rank-table core (ext `rankbadge.c` / `rank13.h`: name key, bounded 0xBF reply parse, lookup / placeholder, symbol / weeks / icon path) |
| `mod/src/net/social/rank-badge.hpp` + `.cpp` | **new** — async 0xBF rank-table poll + `RankUi` widget drawing (ext `rankbadge.c`), consumed by friends13 rows; no ext cutover |
| `mod/src/net/social/friends13.hpp` + `.cpp` | **new** — in-game FRIENDS / PARTY panel, toasts, buttons, party gate + search gate (ext `friends13.c`), rank badge row reservation + MATCH FOUND roster tabs / names |
| `mod/src/engine/widget.hpp` + `.cpp` | extend — mouse navigation (ext `UiRectSs` / `UiMouseOver` / `UiOverViewport`): transform-rect "escape bounds" hover fallback, prologue-checked pinned RVA, `t2r-no-mouse-nav.txt` opt-out |
| `mod/src/engine/window.hpp` + `.cpp` | extend — `KeyDown` / `CursorCanvas` helpers (ext `GetAsyncKeyState` / `UiCursorCanvas`) |
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
