# Phase 1 — Enabling infrastructure

Phase 0 ([FEATURES.md](FEATURES.md)) listed the feature modules and tagged each with the
mechanisms it sits on: 135 at this branch point, **154 after build 191** on `main` (19 new direct
modules plus 3 nested `customize_*.c`). Phase 1 ports those **mechanisms** — once each, in `mod/`
idiom — before any feature is ported. A feature port is then "use an existing mechanism + write
the behaviour", not "re-invent native invocation / widget spawn / PCM playback every time".

Rule of this phase (from the Great Merge reframe): the ext module is the **behavioural reference**,
never code to move. We reimplement the mechanism natively; where a mechanism already exists in the
mod, the deliverable is *verification + extension*, not a rewrite.

**Branch state (2026-11-21).** These notes describe the port committed on `desloppify/code-health`
(`5c47def` adds the table-driven localisation support; the earlier `UNTESTED:` commits port the rest).
The branch is deliberately **not merged** into `main`: `main` has advanced with the `Port build 191: …`
commits. Both docs now **inventory** that build-191 ext work (19 new direct modules, 3 nested
`customize_*.c`, 12 new headers, six new private generated headers, 186 new `t2r-*.txt` strings,
regenerated Polish tables; see §12 and FEATURES.md), but none of it is ported here. The ext and mod
codebases differ massively in casing (`snake_case` vs `PascalCase` / `g_camelCase`), so a text merge
is not tractable — build 191 stays a separate behavioural reference and its behaviours are ported
one at a time, as below. No ext cutover or `t2r-*.txt` gate was flipped for the work on this branch.

## Mechanism status

| # | Mechanism | Ext reference | `mod/` today | Gap |
|---|---|---|---|---|
| 1 | Native invocation (DC lookup → call) | `DcCall` / `*(rec+0x10)` | **exists** — `DC::Call`, `DC::Call_V2` | cached-native helper; convention doc |
| 2 | Net event channel | `SendEvent` (winmm `0x18005DC60`, opcodes `0x41..0xAF`) | **exists** — `NetEventType` + `net::session::SendEvent` | slot reservation + exact-byte compatibility |
| 3 | Widgets / HUD | `GUI2_FIND/CHILD`, `gui2-rxfm-*`; build 191 adds the `nrui.c` tab frame + `customize` / `achieve_ui` / `mfnr` / `lbnr` / `pmunlock` / `namerim` / `fac13ui` pages | **partial** — `GUI2::SPAWN_WIDGET`, `MENU2::*` | widget-tree nav, transform, draw helper, `mod/widgets/` source; every build-191 page unported |
| 4 | PCM audio mixer | `waveOut*` in `loadseq.c`, `t2r-ext.c`; build 191 adds `vintage` / `voxchar` / `sendoff` and the `ach_chime` / `choose_vox` / `choose_music` / `revive_nr` cues | **partial** — `engine/pcm.*` (stream + voices + gain model); stings in `net/audio/*` | 3D voices, loop / crossfade, data wiring; the new build-191 cues are unported |
| 5 | Camera control | render-cam switch + `*_tracks.h` | **partial** — `CAMERA::SET_CAMERA_POSITION/OVERRIDE_FOV`; read-only `engine/render-camera.*` observer (ext icam capture) | camera write path + track evaluator |
| 6 | Pad injection | `padinject.c` (writes `Pad` before `Joypad::Update`) | **ported** — `engine/pad.*` (virtual presses / sticks via mid-hooks at rel32-scanned call sites; command-bit learning; activity stamp; dev-bot consumers still unported) | call-site scan needs live-exe verification |
| 7 | Generated data pipeline | 84 headers (`tools/ext/GENERATED.md`; +6 private in build 191, + `vintage_bolt_built.h` via `__has_include`) | **partial** — `mod/cmake/*.py`, `mod/scripts/make-xvag.py` | wire ext generators / stub strategy / build-time header guard |
| 8 | Config, toggles, logging | 1,636 `t2r-*.txt` + `xlog` in this branch's ext; build-191 adds 186 new strings | `TO1_LOG` + compile-time `config.hpp` | toggle policy + runtime config, then reclassify the new build-191 toggles |
| 9 | Engine RVA call / hook | `*(exe + rva)(…)` + hand-built RWX trampolines (`bp_pick`, `padinject.c`) | **partial** — `engine/transform.*`, `engine/pcm.*`, `engine/backpack-prop.*` call pinned RVAs; `net-patches.cpp` / `loader/boot.cpp` / `engine/pad.cpp` hook via safetyhook | pattern resolution + a shared exe-call helper + version guard |
| 10 | Localisation | `loc.c` / `loc_pl.h` (lookup + language detection every 2 s) | **ported** — `engine/locale.*` (language registry + table-driven lookup, exact / `%d`-`%s` templates / `{1}` captures / `{N|…}` plurals / rich-text split / diacritic fold), `engine/loc_table_pl.hpp` (ported Polish table), `net/social/friends-state.*` (`Tr` uses the active table) | engine text-lookup probe for `auto` detection is not wired in mod/ yet (auto resolves to English); only the Polish table ships, other languages resolve to English until their generated table is added |
| 11 | Tabbed page frame / composed outfits (build 191) | `nrui.c` (`nr_style.py`), `customize_compose.c` (`MSG_CUSTOM_LOOK`, `cz_body_*`) | **not started** — no frame API, no look-wire slot, no composed `look2` consumer | needs §3 widgets, a reserved net slot and `mod/widgets/`; no cutover |
| 12 | Built-header guard (build 191) | `build_ext.py check_built_headers()`, `__has_include("vintage_bolt_built.h")`, `T2R_EXT_NO_BUILT_H=1` | **partial** — `T2R_DATA_*` compile gates and stub strategy exist | wire a mod build-time presence check for generated headers |
| 13 | Remote-puppet playout (netsmooth) | `netsmooth.c`: msg 5/7 pairing, jitter buffer, adaptive delay + playout clock, lerp/slerp + extrapolation, error correction, 10 s `net stats:`; winmm apply/alpha/compose hooks + pinned VAs | **ported (live apply)** — `net/game/net-smooth.*` pure core + `net-smooth-bridge.*` native clock/stats; feeds in `net-session.cpp`, locator cutover in `net-replication.cpp` with an unprimed fallback | ext fixed-VA patches / winmm hooks / send cap / relay dedup / host-bw / root-vec4 / anim-clock advisory have no native equivalent; no live game A/B yet |

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
- `net-protocol.hpp` is the SINGLE registry for every byte on the wire: peer `NetEventType` values
  plus `NetDirectOp` (direct-melee helper), `NetDaemonOp` (notice / requeue / rank / friends / JIP
  / AFK tail) and `NetBackpackPayload` (phase / recorded-track chunk). Feature modules alias these
  enums instead of pinning a second copy of an ext opcode. Values are never renumbered.

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

**Build 191 adds (unported).** `nrui.c` is a shared No Return page frame (breadcrumb, Q/E tab strip
+ extension tabs, week block, boxed prompts, `nr_style.py`) that hosts the redesigned `mfnr.c`
(map vote), `lbnr.c` (leaderboard), `clan13.c` (No Return frame), `customize.c` +
`customize_challenges.c`, `achieve_ui.c` (6th CLAN tab from `achieve_widgets.py`), `pmunlock.c`
(post-match unlocks) and `fac13ui.c` (2013 Factions header). `choose.c` is the redesigned CHOOSE
YOUR SIDE (key art + truck camera, back fix), `sendoff.c` the menu send-off, `namerim.c` the name
outline over the rank-badge rows, and `privlobby.c` the private-match lobby + settings. All need the
tree/transform layer above plus widget defs translated from `nr_style.py` / `*_widgets.py`; none is
ported and no ext page is cut over. The `rankbadge.c` role field and `nrim_tick` rename are new work
on top of the existing `rank-badge.*` port.

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
`boot::FrameTick`). The listener now prefers the final render camera captured by the new
`engine/render-camera.*` read-only observer (the ext's icam filter hook), with the
camera-through-reticle locator + `camera-get-dir-through-reticle` native as fallback -- the real pair
ext's `CamPosDir()` uses for music-sight (ext's own SfxSpatial native tier was dead) -- and the
own-body-at-head-height fallback; `t2r-no-render-camera.txt` makes the reticle pair primary. First consumer: the blade SFX (`net/audio/bladesfx.*`), played 3D on the blade's body.

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
tables. The camera-frustum / LOS "seen" test and the 5–8 m forward cone now have a native producer
in `net/audio/music-sight.*`: the final render camera from `engine/render-camera.*` (the ext's
primary) gives the ext's `cp`/`cd`, with the reticle locator + direction native as fallback --
matching the ext's `CamPosDir()` pair, including its direction normalization;
the peer's exact puppet locator + 1.1 m gives the chest (the same `exe+0x133DB30(proc+0x100)` read
ext makes through `EntityFromHandle`), and extinction is decided by a local port of the ext's tag
body-visibility job (`g_tagHidden`, 12 weighted static-world rays, read back next frame) plus the
ext's `TagScreen` on-screen box (signature-checked engine attachable projection with the
`world-space-to-screen-space` native as fallback; stance from the wire's `isCrouched` / `isProne`
flags = ext's peer+0x85 / +0x86). It feeds the tier's `SetSightSampler` / `SetConeSampler` seams. It
is **enabled by default** with `t2r-no-music-sight.txt` as an opt-out. The render camera observer is
read-only (the write path / track evaluator in §5 stays unported), so live A/B is still advised.
`t2r-no-render-camera.txt` makes the reticle camera primary; `t2r-no-tag-attach-proj.txt` forces the
native projection. Opting out of music-sight clears the two
samplers, in which case the inputs answer "not seen" and the tier keeps its conservative mark /
close-quarters floor.
The ext's enemy-aware *event* values are pure helpers in the same module (`ShotHeat`, `NearFireHeat`,
`CloseHeat`, `DownHeat`, `ReviveHeat`, `StunHeat`, `WeStunnedHeat`). Every event input is now wired:
`we hit an enemy` from `ReportLocalHitOnPuppet`, `spotted by an enemy (spot call)` from an enemy's
received mark line in `net::vox::HandleLine`, `enemy shot heard` / `enemy fire continues` from the
peer shot-count radar, `we are stunned` from `gameplay/stun-lock.*`, and down / revive from the world
scan. `enemy stunned` is fed by two producers: the `NEVT_STUNNED` (ext `MSG_STUNNED` 0x45) receiver
and a per-frame `npc-is-stunned?` poll of the peer puppet in the world scan (the ext uses only the
replicated message, so the poll is a documented divergence that makes the input live without a
local stun broadcaster). Both stamp a 3.5 s deadline; the rising edge is at `d < 40.0f`, as in ext.

**`recv_hit_hook` + immersion near miss (ported).** The damage event now stamps a per-slot hit
time before the alert-sting / music block (`g_hitByMs[g_lastHitSlot]` in ext `RecvHitHook`), keeping
the ext's order: `AlertstingCombat("shot / hit out of the quiet")` then `MusicEvent("we took a hit")`.
`net/audio/near-miss.*` ports the live ext producer: the shooter replicates the line its shot flew
(`NEVT_SHOTRAY` 0xAF, 17 bytes, pinned to the ext's wire), and the receiver computes the segment's
closest approach to its head (t >= 1 m from the muzzle, lateral <= 2.5 m — the live ext value, not
the stale header's ~1.5 m), with a 120 ms delay, a 150 ms whizz cooldown, a 400 ms freshness gate,
an async LOS gate and a 250 ms hit-cancel from that slot. It calls
`AlertstingCombat("near miss out of the quiet")` even when the PCM voice fails (ext behaviour).
Sender is `t2r-no-shotray.txt`, playback `t2r-no-near-miss.txt`, independent as in ext. The whizz
wave table is private (`sp_bullet_by.h` / `sp_bullet_by.pcm`), so playback stays behind
`T2R_DATA_BULLET_BY` while the alert-sting and geometry paths are always live. The geometry and wire
helpers are pure and covered by the standalone `near_miss_core_test`.

**Shiv-victim chain + exec music (ported cores, gated producers).** `gameplay/stun-lock.*` carries
the ext's `g_stun_until`: 3.5 s lock, 0.35 s blend-out, 1.2 s cough cadence, the 1.2 s post-end
grace and the exact front / back shiv victim check (front requires a stunned victim; back accepts
`dd <= 3.5 m` with `ang >= 105` or `ang <= -125`, or a stun). `net/audio/music-hold.*` owns the
ext's `g_msHoldUntil` / `g_msResumeAt` / `g_msNowatchUntil` / `g_msDead` plus `MusicPlay`'s
`FNV64("*name*")` sid. `net/audio/exec-music.*` ports the pending cue timing: the hit seconds
2.44 / 2.35 / 2.31 / 2.35 / 2.39 s land on the death screen (kill + 83 ms delay + 70 ms cut), a
12.5 s cap, a 300 ms fade and a 1.5 s resume. `net/audio/xcam.*` ports the victim-side
`xcam_start` routing: role 2 only, upgraded pipe-mod one-shots suppress exec music, shivs except
`shiv-ground-execution-*` route to the sting / drum cut, `ctxdown` routes to neither, and any other
victim-side move starts the exec cue. `shivsting`/`alertsting` now read the stun core for the
back-shiv alert gate, and `shivsting` cuts / holds combat music through `music-hold` on a shiv
victim start. Within this group every producer is a sink or an inert bridge: no PCM cue, DC music
call or stun-network producer is wired, so ext remains the behaviour owner for those modules. The
new final render-camera observer (`engine/render-camera.*`) is read-only and does not drive any cue
here; the camera-writing path / track evaluator in §5 is still unported. The standalone
`audio_core_test` harness covers the four cores without game headers.

**Build 191 adds (unported).** `vintage.c` (Part I Vintage Rifle gunshot from `p1-vintage.pcm`,
falling back to the hunting rifle event; re-timed bolt copies from `vintage_bolt_built.h`),
`voxchar.c` (headless / KELLIE bodies borrow their faction's voice group), `sendoff.c` (birds /
crows after the war horn), plus the new private `ach_chime.h`, `choose_vox.h`, `choose_music.h`,
`revive_nr.h` and `sendoff_pcm.h` cue tables. They sit on the same `engine::pcm` mixer; none of the
triggers or blobs is wired in the mod, and `loc.c` / `padinject.c` / `glass.c` / `afk.c` /
`requeue.c` had no build-191 behaviour change.

**Gap.** Loop points and the crossfade used by `downed_music`, and the data wiring (item 7): the PCM
blobs are private generators, so `net/audio/*` compiles with the `T2R_DATA_*` gates off and
`Start()` / `Play()` are no-ops until the headers exist. The *routing* for the shiv / exec cues is
ported in `xcam.*`, but no native `xcam_start` hook exists to call it; the shiv drum / exec PCM
producers and the DC `MusicPlay` sink stay inert, and the stun producer is absent, so the ext still
owns those behaviours. The remaining blade *triggers* (the melee-label hooks) are also unported.

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
The new `engine/render-camera.*` is a **read-only** capture of the final render camera: a
safetyhook mid-hook at `FUN_141477750 + 15` (past the ext's 15-byte prologue detour) snapshots the
final position + quaternion forward into a seqlock that `engine::pcm` and `net::audio::music_sight`
consume. It never writes the camera, so the ext stays the behaviour owner; opt out with
`t2r-no-render-camera.txt`.

**Gap.** The render-camera *write* path the ext needs (modifying the final camera, not the script
camera) and a track evaluator that can play a generated track and blend back. FOV continuity. The
read-only observer does not close this gap.

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
the behaviour owner and no toggle is flipped. The flag-0x04 late-join query is ported: four
tries 1.5 s apart in substates 4/5/6, the neutral `JipAsk(4, ...)` fields and the wave-byte
bit 1 / bit 2 answers (`net::jip::LateAnswer()` / `BackAnswer()` / `KitCarryDecision()`); the
kit-carry take / give half (upgrades, armour, purchasable, utilities, parts) stays in the ext.
Requeue (`0x20`) is ported (`net/online/requeue.*`), so the report sends that flag
while `t2r-no-requeue.txt` is absent.

**Requeue and the direct melee path (ported with honest gates).** `net/online/requeue.*` owns
the 0xBC / 0xBD ticket poll, the post-match arm and settle, the `to1/find-match-menu` push and
the WAITING FOR PLAYERS... label. `net/game/net-direct.*` owns the helper loopback socket and
the direct message / ping / pong wire, with the 800 ms dedupe ring hooked into the existing
event channel at `SendEventRaw` and `HandleState`. Both use the same `t2r-*.txt` sidecars as
the ext. Requeue's post-match screen gate is native now: `net/ui/postmatch-screens.*` samples
the menu stack for the clan13x scoreboard, clan-postmatch counter, clan-event screen, GEAR UP
navigation and the `g_mmBack` refused-search latch, and requeue's `PostMatchScreensDone()` is a
single call into it. The ext's `g_pm.pending` / `g_ev.pending` decisions and pre-push latches are
native via `net/online/clan-state.*` (the 0xE0/0xE1/0xE2/0xE4 RPC, `EventWanted` / `PmReady` and
the report retry); the 2013 CLAN page data (0xB6 ask / 0xB7 reply) is native via
`net/social/clan-page-*`; GEAR UP's `g_loState` transitions are native via `postmatch::NoteLoadout*`
and `NoteCountdownChoose`; and both `g_mmBack` producers (party gate and Steam helper not
connected) are native via `net-gui2.cpp` / `net/online/steam-status.*`. Only the widget page
push/render stays ext-owned. Net-direct's `Eligible` now mirrors the ext's
`NdTypeOk` exactly: the reserved melee opcodes (`0x6C`, `0x97..0x9A`, `0x9C`, plus PSYNC `0x4B`
and MV3 `0x9E` minus their PING / PONG) take the direct path too, so a ported client and an ext
client stay interoperable. This is a transport decision, independent of the now-live melee bridge.
Requeue's kick signal is now native (see below).

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
- **clan-postmatch** — the supply-counter page (`to1/clan-postmatch`; ext `g_pm.pending`). The
  decision and pre-push latch are native via `net/online/clan-state.*` (`PmReady` + the 0xE2
  report retry); the page itself stays ext-owned.
- **clan event** — the announcement / result screen (`to1/clan-event`; ext `g_ev.pending`). Native
  too: `EventWanted` on the 0xE1 state decides the offer (a played result first, then the
  announcement) and `net/online/clan-state.*` latches it before the ext pushes the page.
- **GEAR UP nav** — any loadout / picker page (`gearup13`, `loadout-menu`, `picker-skill` /
  `-primary` / `-secondary` / `-purchase`), the observable equivalent of ext `g_loState != LO_NONE`.
  `NoteLoadoutOpen` / `-Edit` / `-Applied` / `-Close` and `NoteCountdownChoose` now drive the
  native `g_loState` (phase 2 / countdown 100..130, back to LO_NONE on countdown end or Esc).
- **g_mmBack** — a refused FIND MATCH holds the settle for three frames (ext `MmreqHook`), then
  the latch clears. Both producers are native: `net-gui2.cpp`'s party-gate branch and the Steam
  helper-not-connected branch (`net/online/steam-status.*`'s 1 s-cached
  `%TEMP%\t2r-steam-status.txt` gate). A styled `net::notify` toast replaces the daemon panel the
  ext relies on.

Each page latch is sampled once per frame from `MENU2::IS_PAGE_ON_STACK`; `ScreensDone()` is the
single aggregate requeue consumes in `PostMatchScreensDone()`, replacing the old `return true`
stub. No page is pushed or popped: the ext still owns the widget page push/render while it is the
bridge, and `kPopOnMmBack == false` because net-gui2 already pops the page it refuses (the mod
shows the refusal as a toast and pops immediately instead of queueing a player the helper cannot
host). The 2013 CLAN page data is native too: `net/social/clan-page-core.hpp` is the pure port of
`tools/ext/clan13.h` and `net/social/clan-page-data.*` owns the 0xB6 ask / 0xB7 reply (bounded
64 KB, section versions and snapshot getters), with only the `to1/clan13` widgets left to the ext.

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
`RosterName`. Build 191's role half is native too: the `:role` column / role-clamped table,
`rkb_role_of` / role-tinted icon + `<font color=...>` weeks text, and a scoped `nrim_tick`
(`net/social/name-rim.*`) for the lead-dev pink lobby-row rim. Click-to-nav is native as well:
the ext's `FrScan` canvas boxes hit-test the rows and footer prompts through the native cursor
canvas; a row click only moves the focus (the engine's mouse-sourced menu-select is dropped for
8 frames, as in the ext), and the hovered row gets the `Fill`/`SetRow` highlight. The exact
`menu-*` pad bindings and the card's per-widget keyframes (`frToast` leaves, 260/220 ms slide +
fade, root pivot) are native. Honest gates: the engine / winmm text writes for the SEARCHING
rows still go through the ext's pinned settext hook (the `g_srchTab` / `g_srchName` mirror only
observes the mod's own writes plus the read-only roster), lobby object writes, the scoreboard's
pink rim (no scoreboard settext note in the mod) and the own-badge clan 0xE1 role stay in the ext;
widget writes are inert when the defs are absent.

**Gap.** No binary / RE notes are available locally to verify the call-site scan or the pinned
RVA prologues against the actual `tlou-ii.exe`; this needs a live build to confirm. The dev-bot
and melee-pace / QTE consumers that *use* the virtual pad (`bot`, `melee_dodge`, the test
harnesses) are not ported — the pad API they would call is present and ready. JIP's roster /
team-swap / ghost-reap **cores** are native now but their engine writes / peer drops are gated
off; the late-join query's wire answers (`0x04`) are native now (kit-carry restoration is still
in the ext). The pinned lobby / peer-record RVA calls need a live exe to verify.

**Deliverable (done).** `src/engine/pad.hpp/.cpp` exposes the full virtual pad; call sites are
resolved by rel32 scan and hooked with safetyhook mid-hooks, not the ext's hardcoded RVAs / thunks.

**Gate (open).** A dev-injected press starts a real melee swing (the ext's acceptance test:
"engine reads 27") — needs the bot / melee-pace consumer, which remains unported.

---

## 7. Generated-data pipeline — partial

**Ext mechanism.** `tools/ext/GENERATED.md`: 84 game-derived headers after build 191 (was 78;
81 are referenced anywhere in the ext, 78 at this branch point) — audio PCM, animation carriers,
camera tracks, move tables, voice ids. Most generators are **private**; a handful are public
(`vox_lines.py`, `gen_vox_words.py`, `scar_draw_gen.py`, `achievements.py --ext-header`).
Build 191 adds six private rows (`ach_chime.h`, `choose_intro.h`, `choose_music.h`, `choose_vox.h`,
`revive_nr.h`, `sendoff_pcm.h`) plus `vintage_bolt_built.h` behind `__has_include`, and
`build_ext.py` now validates built headers with `check_built_headers()` (escape
`T2R_EXT_NO_BUILT_H=1`) and passes `T2R_SLOTS` as `-DT2R_MAX_PEERS`. The ext cannot build without
its generated headers; the mod must not gain that hard dependency.

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

- **Toggles.** 1,636 distinct `t2r-*.txt` strings across 124 modules at this branch point; build
  191 adds ~186 new strings (`t2r-no-achievements.txt`, `t2r-no-bodyshield.txt`,
  `t2r-no-customize.txt`, `t2r-no-customize-look.txt`, `t2r-no-customize-stage.txt`,
  `t2r-no-private.txt`, `t2r-no-namerim.txt`, `t2r-no-netdiag.txt`, `t2r-no-level-cull.txt`,
  `t2r-no-sendoff.txt`, `t2r-no-dodge-tail.txt`, `t2r-no-pin-byst-windup.txt`,
  `t2r-pin-victim-keeps-weapon.txt`, `t2r-revive-2013.txt`, `t2r-dragon.txt`, `t2r-vintage-p1.txt`,
  …). Policy for Phase 1: classify each as (a) *release behaviour* → reproduced unconditionally,
  (b) *dev/debug* → dropped, (c) *tuning* → a mod config value. Do **not** port the file-probe
  mechanism or flip any ext gate for the build-191 ports. Mod already has compile-time
  `src/loader/config.hpp`; add a small runtime config surface only if a tuning knob needs to survive
  to release.
- **Logging.** Unify on `TO1_LOG` (`src/log.hpp`, compiled out in `TO1_RELEASE`). Each ported feature
  drops its ext `xlog` / `t2r-ext-boot.txt` trail and uses `TO1_LOG` with the same prefix (so the A/B
  grep still works).
- **Language selection.** `engine::locale` resolves the active language from `T2R_LANG` or a one-line
  `t2r-lang.txt` next to the exe (`en` / `pl` / `auto` / any registered code), re-read every 2 s.
  There is no toggle to port here: the existing sidecar names are kept for compatibility, and the
  0/1 Polish flag sent to the daemon (`friends13`, `notice`) still uses `IsPolish()`.
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
  and press / dodged verdicts. Also the ported window narrowing: melee-tell `WinMs/ReactMs`, the
  `MexWindowEnd` / `MduWindowEnd` overrides, the pure `GeometryOk` (3.6 m / ~40 deg, loose
  4.4 m / 0.5, 1.3 m height, 450 ms turn grace) and the late-press hold (`g_mdh`, grace
  `round(oneway + playout)` clamped **40..90 ms** -- the ext's current code, not its older 60..180
  comment) with held-blow replay. Gate: pad injection is available (`engine/pad.*`) but the swing /
  contact / animation / prompt producers, the peer pose feed and the relay wire are not wired to it
  yet; the bridge supplies the exchange / duel window callbacks, not the geometry callback.
- **Melee tell** — `gameplay/melee-tell.*`. Pure timing core: contact at the tell (320 ms default,
  `200..500` range), x1.0..x1.25 windup speed-up, the 0.5 EMA contact estimate per weapon class and
  `WinMs = tell + 120` / `ReactMs = tell` (0 when opted out or melee v2). The audio / clip cue half
  stays gated; the host supplies the ext's file-toggle state through the setters.
- **Melee duel fair window** — `gameplay/melee-duel.*`. Pure duel window (`MduWindowEnd`: contact
  cap, 400 ms swing-match tolerance) and next-press fairness floor (`VicExitMs` 650 ms / rate 1.12,
  `VicLead` 54 - 2*oneway + 2*oneway + 10, +40 slack, capped at base + 60 ms / 900). The duel clip
  / -hit pair / psync / wire producers are not ported, so the core stays inactive until fed.
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

**Integration seams (landed, live).** `net/match/melee-bridge.*` is the single place that
installs one sink per core and fans out the frame tick (`boot::FrameTick`), the reserved `NEVT_*`
receive cases (`net-replication::HandleNetEvent`) and the session reset
(`net-replication::ResetRuntime`). With `melee_bridge::kEnabled == true` the sinks use the real
clock / slots / per-peer `PingMs()` and send the exact reserved opcodes over
`net::session::SendEventRaw`: hitrel's reliable `0x6C` (with the plain `NEVT_TAKE_DAMAGE` /
`NEVT_ARMOR_HIT` fallbacks), the dodge `0x97` / `0x98` / `0x99` messages, hitreact's `0x9A` and
the exchange's `0x9C`. Reliable hits re-enter `HandleNetEvent(NEVT_TAKE_DAMAGE)` so the normal
damage bookkeeping / stings / music inputs / lethal claim / `FireAttack` all run once. Local
producers are wired into the existing hooks: `ReportLocalHitOnPuppet` routes through hitrel and
arms `OnBlowDealt`, `hk_TryAttack` announces `0x97`, `NEVT_PLAYER_MELEE_ATTACK` feeds
`melee_dodge::NoteSwing`, `NEVT_TAKE_DAMAGE` feeds `OnBlowTaken`, and the bridge's `Tick()` feeds
the local dodge press edge from the raw dodge pad bit. Reaction clips / pain VO on the receive
side animate / say on the sender's puppet by sid. Still gated (no native producer): the private
hit-react clip table (`PickClip`), punch-at-joint / blade FX confirmation, the QTE system, the
exchange's engine-held contact replay, MV3's `Mv3nIo` + glue, and PSYNC's wire core — those sinks
return conservative defaults. The ext copy still owns those effects; no ext cutover and no
`t2r-*.txt` toggle was flipped.

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

## 12. Build 191 — new ext work and rework of ported features

`main` gained 19 direct ext modules (+3 nested in `customize.c`) after this branch point. FEATURES.md
has one row per file; the mechanism-level split is:

- **UI / pages (§3):** `nrui` (shared tab frame), `mfnr`, `lbnr`, `customize` +
  `customize_challenges` / `customize_compose` / `customize_stage`, `achieve_ui`, `pmunlock`,
  `fac13ui`, `choose`, `namerim`, `sendoff` — all wait on the widget tree/transform layer and
  `mod/widgets/` defs. `customize_compose` additionally needs a reserved `MSG_CUSTOM_LOOK` slot and
  a `cz_body_*` consumer; `namerim` and the new rank `role` / `nrim_tick` belong to the already-ported
  `rank-badge.*` seam.
- **Audio (§4):** `vintage` (with `vintage_bolt_built.h`), `voxchar`, plus the new private cue tables
  `ach_chime` / `choose_vox` / `choose_music` / `revive_nr` / `sendoff_pcm`.
- **Gameplay / net:** `achieve` (daemon 0xA9 report / 0xAA table, `ach_live.h`), `privlobby`
  (0xF8/0xF9, `pm_*` rules), `dragon2013` (`MSG_DRAGON 0xB7`), `bodyshield` (pure rules in
  `bodyshield.h`, escape QTE / mash meter on the pad API), `levelcull` / `ldcwatch` / `netdiag`
  (diagnostics).
- **Deleted dead layer:** `t2r-engine.c/.h/-symbols.def` — no publisher existed, main uses
  `VMA()` / `dc_call(fnv64(...))` / `hookreg.h`. Nothing to port or preserve.

**Rework of features already ported here** (their FEATURES.md rows carry `build 191:` notes):
`friends13` (No Return redesign + toast kinds), `rankbadge` (roles), `backpack` (only the
`bp_look_prop` → `cz_bag_look_sid` hook in `t2r-ext.c`, not the module body — the hook **is** ported
as the inert `engine::backpack_prop` composed-bag seam; its `customize_compose` producer is not),
`ctxdown`
(pin-victim weapon hiding, knife / female counters), `paired` (bodyshield / `revive_nr` /
duel-tail hooks), `revive2013` (kneel help-up), `clan13` (No Return frame), `gearup13`
(scroll / classes, Flamethrower off sale), `modepick` (private row), `sbteam` (stat columns),
`clancard` (pause layout), `clanend` (choose music), `melee_duel` (LOS gate),
`melee_duel_pin` (bystander windup), `melee_duel_sptl` (dodge tail), `melee_dodge` (ps56 counter
window / `ach_dodge`), `melee_pullout` (fail-kill / downed / abort toggles). `loc_pl.h` was
regenerated (`12-customize` / `13-achievements`, restored unsourced lines, Spanish melee skills):
`engine/loc_table_pl.hpp` must be regenerated from main's table to stay in sync. `afk`, `requeue`,
`notice`, `netdirect`, `loc`, `padinject` and `glass` have **no** substantive build-191 change —
their raw-diff rows are comment/casing reversions only.

Honest gate: none of the above is a cutover. No ext toggle was flipped, no ext wire ID was
renumbered, and every new producer stays with the ext until its native counterpart lands.

---

## 13. Remote-puppet playout — netsmooth ported, live cutover

**Ext behaviour (build 191 reference).** `tools/ext/netsmooth.c` wraps winmm's remote-puppet
placement with a per-slot jitter buffer. Each msg-5 state (pos `peer+0x68`, quat `+0x74`) is
stamped with the sender time carried by the msg-7 AnimCmd list of the same sequence. A 4 s
least-delay offset (`oref`) and a jitter peak (`jpk`) drive a playout clock
`R = now + oref - delay`; the puppet is lerped/slerped between samples bracketing `R`, with
capped extrapolation, smooth error correction, a 100 ms handover from winmm's own placement,
and a 10 s `net stats:` line.

**Native port.** `net/game/net-smooth.hpp/.cpp` is an engine-free, single-threaded port of that
core: 8 slots (`0..7`, slot 0 = host, matching ext `T2R_MAX_PEERS`), a 64-sample ring, the
32-entry msg-7 pairing ring, sender-clock rebase + 15.625 ms dequantisation (least squares
through 24 pairs), jitter/loss EWMAs, the adaptive delay + stable `dcur`, the `R` clock,
bracketing interpolation, `NsVel` extrapolation with capped angular rate, the E/hold-Q error
correction and stutter metrics. It uses the caller's double-ms timeline; no engine headers,
no wire changes, no ext toggle read.

`net/game/net-smooth-bridge.hpp/.cpp` supplies `engine::frame::NowMs()` as that timeline,
`NetPlayer::PingMs()/2` as the ext `PsyncOneway` prediction lead, the 10 s `[NETSMOOTH]` log
(ext's line 2104), and the live apply. Feeds are in `net-session.cpp`: `OnState` right after
`PushState()` in `HandleState`, `OnAnim` after the `sendMs` decode in `HandleAnimCmd` (native
State and AnimCmd share `g_myStateSeq`, so the ext's msg-5/msg-7 pairing is preserved).
`net-replication.cpp::ApplyRemote()` now writes the smoothed transform to
`LOCATOR::SET_OBJECT_LOCATOR`; the previous native extrapolation is retained only as the
fallback while the ring is unprimed / a sender clock changes. `replication::Tick()` rolls the
stats window and `ResetRuntime()` clears all slots.

**Out of scope (no native equivalent, need a live exe):** the winmm apply/alpha/compose hooks
and fixed-VA code patches (`+0x68/+0x74/+0xc5`, time ring `+0x2820/38/3c`, reset bit
`+0x77e4`, anim clock `+0x74c8`), the anim-clock advisory `g_nsAnim`, the send cap / relay
dedup / host-bw LOD, the five root-vec4 interpolation (not present in `PlayerNetState`) and
the dev netsim. The native anim layer keeps its own 25 ms play clock; this port does not couple
to it.

**Verification.** `mod/tests/netsmooth_core_test.cpp` is a standalone C++17 harness (feeds +
sampling, pairing, teleport detection, dequantisation, sender-clock rebase, msg-7 loss,
handover blend, unprimed fallback) that passes here; the bridge compiles syntax-only. There is no game build here, so the live A/B against the ext and the
`[NETSMOOTH]` vs `net stats:` comparison are still outstanding; the fallback is not an enable
gate, and no ext toggle or wire ID was touched.

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
| `mod/src/net/protocol/net-protocol.hpp` | ext `NEVT_*` slot values + payload structs + central `NetDirectOp` / `NetDaemonOp` / `NetBackpackPayload` opcodes (all ported wire bytes, exact ext values) |
| `mod/src/net/game/net-replication.cpp` | dispatch `case`s for ported slots; **netsmooth live cutover** — `ApplyRemote` writes the smoothed transform to `LOCATOR::SET_OBJECT_LOCATOR` (native interpolation retained as unprimed fallback), `Tick()` rolls the 10 s stats window, `ResetRuntime()` clears slots |
| `mod/src/net/game/net-smooth.hpp` + `.cpp` | **new** — pure remote-puppet playout core (ext `netsmooth.c`): 8-slot 64-sample ring, msg 5/7 pairing, rebase + 15.625 ms dequant, jitter/loss/delay, playout clock, lerp/slerp + extrapolation, error correction, stats; engine-free, single-threaded |
| `mod/src/net/game/net-smooth-bridge.hpp` + `.cpp` | **new** — native bridge: `frame::NowMs()` clock, `PingMs()/2` lead, 10 s `[NETSMOOTH]` log, live apply; state/anim feeds wired in `net-session.cpp` |
| `mod/tests/netsmooth_core_test.cpp` | **new** — standalone C++17 pure-core harness (no engine headers, not in CMake); run command in the file header |
| `mod/widgets/*.widget.json` | **new** — widget defs translated from `tools/features/*_widgets.py` |
| `mod/cmake/gen-*.py` + `mod/CMakeLists.txt` | data-header codegen template + wiring |
| `mod/src/loader/config.hpp` (+ runtime config if needed) | toggle policy surface |
| `mod/src/engine/ptr.hpp` | `Readable` / `Writable` guarded pointer reads (ext `IsBadReadPtr` / raw deref) |
| `mod/src/engine/lobby.hpp` + `.cpp` | **new** — read-only engine net lobby (ext `MatchSubstate` / `LobbyObj` / roster + peer-record + `TeamOf`): substate 6, `+0x2ca4` end banner, clock, roster helpers; `RosterSorted` (ext `LobbyRowsSorted`: our team blue first, enemy red) + `RosterName` for MATCH FOUND friends rows; no writes |
| `mod/src/engine/frame.hpp` + `.cpp` | QPC ms clock (`NowMs`, ext `BpMs`) alongside the 60 Hz frame counter |
| `mod/src/engine/object.hpp` + `.cpp` | **new** — entity / joint locators, draw-control mesh hide/show, set-locator (ext `XbxLoc` / `GvisDc` / `BpDc` / `PsyncBfFromLoc`) |
| `mod/src/engine/anim-prims.hpp` + `.cpp` | **new** — `AnimateFrom` / `AnimLen` (ext `animate_` recipe + `get-animation-duration`) |
| `mod/src/engine/backpack-prop.hpp` + `.cpp` | **new** — own-side backpack hand-off (ext `feature_backpack_prop`); build-191 `bp_look_prop` → `cz_bag_look_sid` composed-bag hook ported as an inert producer seam (`SetComposedBagLook` / `ComposedBagLook`, 16 slots; no composer registered) |
| `mod/src/engine/armor-rig.hpp` + `.cpp` | **new** — skinned-armour tables + `look2` resolver + per-body apply (ext `ARMOR_RIG_*` / `ArmorRigApply`); the armour-visual driver is unported |
| `mod/src/net/match/backpack.hpp` + `.cpp` | **new** — TAB backpack wire + prop state machine + engine prop spawn (ext `backpack.c` + `bp_pick`) |
| `mod/src/engine/clipfx.hpp` + `.cpp` | **new** — engine `part-effect` anim-event fire path (ext `clipfx.c`) |
| `mod/src/engine/vfx.hpp` + `.cpp` | **new** — particle spawn + compute-entry / retry (ext `CfxSpawnRaw` / `ArmorFxSpawn` / `CfxRetryTick`) |
| `mod/src/gameplay/melee2013.hpp` + `.cpp` | **new** — 2013 melee move table (ext `MELEE2013` / `MxMove` / `MxByMove`) |
| `mod/src/gameplay/ctxdown.hpp` + `.cpp` + `ctxdown-rows/*.h` | **new** — contextual-down pair table (ext `ctxdown.c` `CD_PAIRS` + `mduel_*` / `pullout_rows.h`) |
| `mod/src/net/anim/paired.hpp` + `.cpp` | **new** — psync paired-move surface + abort hook (ext `paired.c` `g_ps` subset) |
| `mod/src/net/match/bloodbuild.hpp` + `.cpp` | **new** — per-life blood / dirt build-up (ext `bloodbuild.c`) |
| `mod/src/net/audio/bladesfx.hpp` + `.cpp` | **new** — TLOU2 blade cues (ext `blade_sfx.h`) |
| `mod/src/net/audio/music-tier.hpp` + `.cpp` | extend — combat music tier STATE + floors + `g_msDead` (ext `MusicEvent` / `FeatureCombatMusic`), injectable sight / forward-cone samplers, all enemy-aware event inputs (hit / spot / shot / fire / down / revive / stun) and event-value helpers; playback stays in the ext |
| `mod/src/net/audio/music-sight.hpp` + `.cpp` | **new** — native `MusicSlotSeen` / `MusicSlotCos` producer (final render camera + reticle fallback = ext's `CamPosDir()`; exact puppet locator chest, ext's tag body-visibility `g_tagHidden` + `TagScreen` on-screen box); enabled by default, `t2r-no-music-sight.txt` opt-out |
| `mod/tests/music_sight_core_test.cpp` | **new** — standalone C++17 harness for the ext frustum / body-visibility / on-screen pure helpers (`MUSIC_SIGHT_CORE_TEST_OK`) |
| `mod/src/net/audio/near-miss.hpp` + `.cpp` | **new** — immersion near-miss producer: pure closest-approach / wire quantisation helpers, `NEVT_SHOTRAY` sender + receiver ring, 120 ms hit-cancelled whizz with alert sting, LOS gate and private-data PCM path; `t2r-no-shotray.txt` / `t2r-no-near-miss.txt` |
| `mod/tests/near_miss_core_test.cpp` | **new** — standalone C++17 harness for the near-miss geometry / wire helpers (`NEAR_MISS_CORE_TEST_OK`) |
| `mod/src/net/audio/music-hold.hpp` + `.cpp` | **new** — pure ext `g_msHoldUntil` / `g_msResumeAt` / `g_msNowatchUntil` / `g_msDead` state + `MusicPlay` sid (`FNV64("*name*")`), behind an inert DC sink |
| `mod/src/net/audio/exec-music.hpp` + `.cpp` | **new** — pure exec victim cue timing (ext `exec_music.c`: `EXMUS_HIT[]` / `EXMUS_CAP_MS` / `EXMUS_FADE_MS` / `EXMUS_RESUME_MS`), gated PCM sink |
| `mod/src/net/audio/xcam.hpp` + `.cpp` | **new** — pure `xcam_start` role-2 routing (upgraded / shiv / `ctxdown` / exec), inert dispatch seam |
| `mod/src/gameplay/stun-lock.hpp` + `.cpp` | **new** — pure stun lock + end grace + front / back shiv victim check (ext `g_stun_until` / `g_stunEndedT60`), network producer gated |
| `mod/tests/audio_core_test.cpp` | **new** — standalone C++17 harness for the stun / music-hold / exec-music / xcam cores and the tier / sight pure helpers (`AUDIO_CORE_TEST_OK`); no game headers |
| `mod/src/net/match/match-liveness.hpp` + `.cpp` | **new** — shared once-per-frame match-end latch (ext `g_wasLive`): engine substate 6 / end banner via `engine/lobby.*` with a mod-phase fallback; `Live()` / `WasLive()` / `MatchOver()` / `MatchEndHold()` consumed by requeue, friends13, `player_status` + AFK |
| `mod/src/net/match/kick-state.hpp` + `.cpp` | **new** — shared kick timestamp / text tests (ext `g_kickMs` / `MsgIsKick` / `MsgIsMove`), fed by the notice kind-'p' line, consumed by requeue + notice |
| `mod/src/net/ui/postmatch-screens.hpp` + `.cpp` | **new** — post-match / menu latches (ext clan13x `Cl13PostActive`, `g_pm.pending`, `g_ev.pending`, GEAR UP `g_loState`, `g_mmBack`): sampled once/frame from the menu stack; `ScreensDone()` is requeue's single gate; native `g_loState` via `NoteLoadout*` / `NoteCountdownChoose`, `g_mmBack` pop seam gated (`kPopOnMmBack == false`) |
| `mod/src/net/online/clan-state.hpp` + `.cpp` | **new** — native 2013 clan state + post-match decisions (ext `ClanT` / `ClanParse` / `PmReport` / `PmReady` / `EvWanted` / `EvOffer`): exact 0xE0/0xE1 state read, 0xE2 report (with retry) and 0xE4 side choice via `net::daemon::Rpc`, `g_pm.pending` / `g_ev.pending` pre-push latches; pure parse / decision core in the header; widget pages stay ext-owned |
| `mod/src/net/social/clan-page-core.hpp` | **new** — pure core of the in-game CLAN page (ext `tools/ext/clan13.h`): bounded 0xB7 row parse for sections 1..6, cards / thread / week / day / match-stat views and text clip / wrap; header-only for the standalone test |
| `mod/src/net/social/clan-page-data.hpp` + `.cpp` | **new** — native CLAN page data producer: 0xB6 asker (token / language / section / seen id / day / cap), bounded 64 KB 0xB7 reply, section versions and snapshot getters; `to1/clan13` widget push/render stays ext-owned |
| `mod/src/net/online/steam-status.hpp` + `.cpp` | **new** — ext-equivalent Steam-only gate: 1 s-cached `%TEMP%\t2r-steam-status.txt` (`<1|0> <unix time>`, 70 s freshness), `SteamOnly()` / `Connected()` / `RefuseFindMatch()`, wired into net-gui2's FIND MATCH refusal |
| `mod/tests/clan_core_test.cpp` | **new** — standalone C++17 harness for the clan state / decision and CLAN page pure cores (`CLAN_CORE_TEST_OK`); no game headers |
| `mod/src/net/match/melee-bridge.hpp` + `.cpp` | **new** — live integration seams: wire / hit / dodge / anim / sound sinks + boot tick / net-event dispatch / session reset fan-out; master gate `kEnabled == true`, local producers wired from the melee hooks |
| `mod/src/net/match/downed-state.hpp` + `.cpp` | **new** — pure 2013 downed core (ext `feature_downed` / `DownedSet` / `RvProg` / `MSG_DOWNED` / `MSG_REVIVE`): state + epoch, the dying latches (`g_downDying` / `g_uc4DyingUntil`) + 0/1/2 `LocalState()` / `PuppetDownState()`, pool / protection / bleed-out clock, multi-reviver aggregate, peer reorder guard, exact wire layouts, behind `DownedSink` |
| `mod/src/net/online/jip-report.hpp` + `.cpp` | extend — 0xE5 roster-shadow merge + 0xA7 own-team swap parse + ghost-peer reaper cores (observation-only; engine writes / peer drops / 0x10 gated false) |
| `mod/src/net/game/player-status.hpp` + `.cpp` | extend — `MatchLive()` reads the shared engine liveness latch; `LocalDownState()` / `PuppetDownState()` expose the 0/1/2 dying distinction to `bloodbuild.cpp` |
| `mod/src/net/match/downed-bridge.hpp` + `.cpp` | **new** — gated integration seam for the downed core: inert sink + boot tick / net-event dispatch / session reset fan-out; master gate `kEnabled == false` |
| `mod/src/net/match/hit-rel.hpp` + `.cpp` | **new** — reliable one-shot hit core (ext hitrel section): seq / retransmits / ACK / dedupe ring, behind `HitRelSink` |
| `mod/src/gameplay/melee-dodge.hpp` + `.cpp` | extend — pure dodge timing / state core (ext `melee_dodge.c`) + tell narrowing, exchange / duel window overrides, `GeometryOk` and late-press hold (`g_mdh`), behind `DodgeSink` |
| `mod/src/gameplay/melee-tell.hpp` + `.cpp` | **new** — pure windup-tell contact / window core (ext `melee_tell.c`: `MtellWinMs` / `MtellReactMs`, `MtellTc` / `MtellSpeed` / `MtellNoteContact`); cue audio / clips gated |
| `mod/src/gameplay/melee-duel.hpp` + `.cpp` | **new** — pure duel fair-window core (ext `melee_duel.c`: `MduWindowEnd`, `MduVicExitMs`, ps45 `Md45VicLead` / next-press gate), behind explicit host state; duel engine / wire gated |
| `mod/tests/melee_timing_core_test.cpp` | **new** — standalone C++17 harness for the tell / duel / dodge window and hold cores (`MELEE_TIMING_CORE_TEST_OK`); no game headers |
| `mod/src/gameplay/melee-hit-react.hpp` + `.cpp` | **new** — pure hit-reaction / attacker-lock / prediction core (ext `melee_hitreact.c`), behind `HitReactSink`; clip table gated |
| `mod/src/gameplay/melee-exchange.hpp` + `.cpp` | **new** — pure exchange state machine / wire core (ext `melee_exchange.c`), behind `MexSink` |
| `mod/src/gameplay/mv3/{types.hpp,sim.cpp,net-core.hpp,net-core.cpp}` | **new** — melee-v3 contract + generated table + deterministic sim + engine-free rollback / wire core (ext `mv3.h` / `mv3_moves.h` / `mv3_sim.c` / `mv3_net_core.h`); names pinned to the shared contract |
| `mod/src/net/social/friends-state.hpp` + `.cpp` | **new** — pure 0xD9 JSON state / rows / actions core (ext `friends13.h`) + rank-table core (ext `rankbadge.c` / `rank13.h`: name key, bounded 0xBF reply parse, lookup / placeholder, symbol / weeks / icon path) |
| `mod/src/net/social/rank-badge.hpp` + `.cpp` | **new** — async 0xBF rank-table poll + `RankUi` widget drawing (ext `rankbadge.c`), consumed by friends13 rows; no ext cutover |
| `mod/src/net/social/friends13.hpp` + `.cpp` | **new** — in-game FRIENDS / PARTY panel, toasts, buttons, party gate + search gate (ext `friends13.c`), rank badge row reservation + MATCH FOUND roster tabs / names |
| `mod/src/engine/locale.hpp` + `.cpp` | **new** — table-driven localisation (ext `loc.c`): language registry keyed by code, English -> per-language lookup with exact / template / plural / capture / rich-text rules, diacritic-folded raw draws, `T2R_LANG` / `t2r-lang.txt` selection re-read every 2 s, `SetOverride` / `Reset` test seams; call sites use `Tr(en[, pl])` / `Translate` / `TranslateDraw` |
| `mod/src/engine/loc_table_pl.hpp` | **new** — generated Polish translation table + probe ids (ext `loc_pl.h`), one row in the registry; adding another language is a new generated header + registry row, no call-site changes |
| `mod/src/engine/widget.hpp` + `.cpp` | extend — mouse navigation (ext `UiRectSs` / `UiMouseOver` / `UiOverViewport`): transform-rect "escape bounds" hover fallback, prologue-checked pinned RVA, `t2r-no-mouse-nav.txt` opt-out |
| `mod/src/engine/window.hpp` + `.cpp` | extend — `KeyDown` / `CursorCanvas` helpers (ext `GetAsyncKeyState` / `UiCursorCanvas`) |
| `mod/src/engine/exe.hpp` | **new** — raw exe-RVA call + prologue-checked safetyhook mid-hook helper (ext pinned-RVA calls / RWX trampolines) |
| `mod/src/engine/render-camera.hpp` + `.cpp` | **new** — read-only final render camera observer (ext icam capture): safetyhook mid-hook at `FUN_141477750 + 15` snapshots position + quaternion forward into a seqlock for `pcm` / `music-sight`; pure `ForwardFromQuaternion` / `QuaternionOk` / `Fresh` core; `t2r-no-render-camera.txt` opt-out; camera write path / track evaluator stay in the ext |
| `mod/tests/render_camera_core_test.cpp` | **new** — standalone C++17 harness for the render-camera pure helpers (`RENDER_CAMERA_CORE_TEST_OK`); no game headers |
| `mod/PORTING.md` | mechanisms section: DC mapping, `MSG_*`⇄`NEVT_*` table, toggle/log/hook conventions |
| `mod/FEATURES.md` | data-table column: generator availability; build-191 inventory — 154 direct modules (+3 nested `customize_*`), 12 new headers / 6 private generated rows, deleted `t2r-engine.*`, and `build 191:` flags on every ported row with new work |
| `mod/src/engine/loc_table_pl.hpp` | regenerate from `main:tools/ext/loc_pl.h` (build 191: `12-customize`, `13-achievements`, restored lines, Spanish melee skills); keep the registry API unchanged |

## Risks

- **No local build here.** Every gate above needs the user's Windows toolchain (clang-cl + LLVM +
  game install); this environment can write/refactor source and scripts but cannot run the A/B.
- **Netsmooth cutover is unverified in-game.** The pure core has an offline behavioural harness,
  but there is no winmm/exe here to A/B remote-puppet motion against the ext. The retained native
  path only covers an unprimed buffer; watch the `[NETSMOOTH]` 10 s line against the ext's
  `net stats:` (delay, extrap %, snaps, stutter) on a live build and revert the four call sites in
  `net-session.cpp` / `net-replication.cpp` if remote puppets stutter.
- **`main` has diverged (Port build 191).** The new ext modules / headers are now inventoried in
  FEATURES.md and §12, but none is ported and the branch is intentionally *not* merged (ext/mod
  casing differs too widely for a text merge). Several already-ported features gained new work on
  `main` (§12), so "ported" statuses must be read with their `build 191:` notes; do not treat the
  wire slots, generated tables or Polish strings here as build-191-current.
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
