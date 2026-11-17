# Due to circumstances outside of our control, the tools CMake requires to build WILL NOT BE SHIPPED. Apologies.
# Porting map: legacy byte patches -> `mod/` source
# Also: this is extremely untested!!!

`mod/` is the source-mod tree: a copy of the client source whose behaviour is meant to match the
shipped, byte-patched `winmm.dll`. Every legacy patch tool under `tools/features/` is expressed here
as a source edit, each carrying a `// was <tool>: <patch-name>` provenance comment. **No legacy tool
was deleted** -- `winmm/`, `tools/features/*` and the fixed-offset patch pipeline are untouched.

The shipped `winmm.dll` is still produced by the patch pipeline (`tools/features/build_dll.py` over
`tools/local/winmm-nocave-127.0.0.1-7001.dll`). This tree is the porting target for the staged move
to a source build; the patch build keeps working until the remaining dependency below is solved.

## Patch -> source

| Legacy patch / tool | Legacy site | `mod/src` port | Notes |
|---|---|---|---|
| `winmm_patches.py: box-ingredients` | `0x18004DAF4` | `net/match/net-player.cpp` `SpawnItemsFromSupplyBox` (`quantity = 3`) | supply-box ingredients drop again, 3 per pickup |
| `winmm_patches.py: box-crowbar` | `0x18004D7CD` | `net/match/net-player.cpp` (`availableItemSpawnTypes ... "crowbar"`) | replaces the smoke-bomb insert |
| `winmm_patches.py: box-crowbar-ungated` | `0x18004D646` | `net/match/net-player.cpp` `ShouldSpawnSmokeBombInBox` (drops the `matchState == Losing` gate) | only the health-kit insert stays gated |
| `winmm_patches.py: supplybox-12-cap/resize/loop` | `0x18004E27F/28C/38F` | `net/match/net-player.cpp` `InitializeSupplyBoxes` (`num_boxes = 12`) | the base hard-coded 4 three times; one constant covers all three |
| `winmm_patches.py: tab-native-backpack` | `0x1800304D0` | `net/game/net-hooks.cpp` `kTabNativeBackpack` (guards the craft-push hook install) | EXPERIMENT, default `false` (mod store) |
| `winmm_patches.py: mark-without-perk` | `0x18001D6C9` | `gameplay/perks/perk.hpp` `Perk::alwaysOn`; `definitions/hawk_eyed.hpp` (`.alwaysOn = true`); `perk-registry.cpp` `Tick` gate | everyone can mark; Hawk-eyed only extends it |
| `winmm_patches.py: version-text` | `0x18012279F` | `net/base/net-common.hpp` `kModVersion` | **string is `"v0.14 BETA"`** (see note) |
| `winmm_patches.py: reticle-marker-default` | `0x180138228` | `net/ui/net-gui2.cpp` `g_reticleValue = 1` | marker widget default |
| `winmm_patches.py: lobby8-*` (6 patches) | `0x180065A9D`.. | `net/ui/net-gui2.cpp` `kLobbySlots = kLobbyRows` (`kLobbyRows = 8`) | find-match lobby list is 8 rows (4v4) |
| `winmm_patches.py: lp-*` (7 patches) | `0x1800A0EA8`.. | `gameplay/loadout/weapons.hpp`, `gameplay/perks/definitions/{hawk_eyed,marathon-runner,steady-hands}.hpp` | 2013 Factions loadout-point costs |
| `ext_loader.py` | `0x180063370` cave | `loader/boot.hpp`/`boot.cpp` `boot::TickExt()`; called at the top of `net/ui/net-gui2.cpp` `UpdateMenu` | loads `t2r-ext.dll`, calls `t2r_tick()` once per frame |
| `slots8.py` (+ `slots8_peers/players/misc.py`) | `.t2r8` section, relocated tables | `net/protocol/net-protocol.hpp` `kMaxNetPlayers = 8`, `kProtocolVersion = 42` (`TO1_SLOTS4` = 4-player escape) | the source is already `kMaxNetPlayers`-driven, so no table relocation is needed |
| `cave_link.py` | n/a | n/a | code-cave linker used by the patch tools; no runtime behaviour |
| `patch_winmm.py` | `0x56402/0x58725` (IP), `0x563F1/0x58742` (port) | `net/online/net-matchmaking.cpp` `TO1_MM_IP_A..D`, `TO1_MM_PORT` | compile-time server retarget (default Nuremberg EU / port 7001) |
| `p1_side_map.py` | `0x1800019A0` hook + `.t2rps` section | `engine/archive.cpp` `MapSideArchive()` + `g_p1SideDesc` in section `.t2rps`; called from `Init` after `VerifyChecksum` | `TO1_P1_SIDE_SIZE2`, `TO1_P1_SIDE_ID` (see note) |
| `stubpool.py` | appended `.t2rstub` section | n/a | reserves executable stubs within rel32 of winmm code for the ext; only meaningful for the patched base |
| `shrink_rsrc.py` | drops the dead zeroed RCDATA from the file | n/a | file-size cleanup of the packed image; no runtime behaviour |
| `build_dll.py` | orchestration | n/a | the pack/patch pipeline itself, not a behaviour |
| `p1_side.py` | build-time data prep | n/a | produces `part1.dat` + manifest; unchanged |

## Notes

- **version-text**: the base binary carried the literal `"v1.08 BETA"`, patched to `"v0.14 BETA"`.
  `mod/src` now sets `kModVersion = "v0.14 BETA"` to match the shipped string. This is a product
  decision -- re-decide the string (and bump it per release) here.
- **p1 side file**: the byte-patch cave was entered right after the loader's FNV check and rewrote
  `s_psarc`/`s_psarcSize` (`0x1801398d0`/`d8`) to span `winmm.dll` on disk + `part1.dat`. The source
  port does the same in `MapSideArchive()`. It also **emits a `.t2rps` section** (via
  `__declspec(allocate)`) holding the same status descriptor, so the ext's `p1s_read_desc`
  (`tools/ext/p1side.h`) still finds the verdict. With `TO1_P1_SIDE_SIZE2 == 0` the mapper is a no-op
  and `status` stays 0 ("not run"), exactly like a build without `T2R_P1_SIDE`.

## Remaining dependency (blocks shipping a source build)

`t2r-ext.dll` (`tools/ext/t2r-ext.c`) is built against the **patched base binary's** winmm layout: it
hardcodes winmm VMAs throughout (e.g. `PA_SLOT 0x18013B2A0`, `BySlot 0x180050FD0`, the peer table,
`SB_STATS`, the statics behind `0x180138090`) and reads two descriptors from winmm sections:

- `.t2r8` (`s8_init`) for the relocated 8-player tables -- **not needed** by a source build, whose
  tables are naturally 8-wide, but the ext's `g_engine_peers`/`T2R_ENGINE_PEERS` and its peer/PA_SLOT
  VMAs still assume the patched image.
- `.t2rps` (`p1s_read_desc`) for the Part I side-file verdict -- **provided** by `MapSideArchive()`.

Because the ext's winmm VMAs are baked in, a `mod/src` rebuild changes every address it expects. Before
the source build can replace the patch build, the ext must be re-pointed at the source build's
addresses (or rebuilt against exported symbols). Until then, the patch build ships and this tree is
the porting reference.
