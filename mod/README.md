# mod: client source (source-mod tree)

This folder holds the C++ source of the multiplayer client for The Last of Us Part II Remastered on
PC. It builds as `winmm.dll`. The game loads that file as its winmm proxy, and the same file also
holds the mod code.

`mod/` is the **porting target**: a copy of the client source in which every legacy fixed-offset byte
patch is expressed as a source edit. The legacy tools under `tools/features/` (and the `winmm/`
source folder) are kept, untouched, and still drive the shipped build. See
[PORTING.md](PORTING.md) for the patch -> source map.

## How the shipped winmm.dll is made (today)

The release build does **not** build from this folder yet. It:

1. starts from a fixed prebuilt binary (`tools/local/winmm-nocave-127.0.0.1-7001.dll`);
2. applies byte patches at fixed offsets (`tools/features/winmm_patches.py`, applied by
   `tools/features/build_dll.py`);
3. adds the `t2r-ext.dll` loader and the data caves, and sets the server address
   (`tools/features/patch_winmm.py`).

Those patches use fixed offsets, so they only fit that exact binary. `mod/` carries the same
behaviour as source edits so the client can eventually be built from source and the patch pipeline
retired. The remaining blocker is `t2r-ext.dll`, which is compiled against the patched binary's
winmm addresses (see PORTING.md, "Remaining dependency"). Until it is re-pointed, the patch build
keeps shipping.

To install the prebuilt binary, see `tools/fetch_base_dll.py` and [CONTRIBUTING.md](../CONTRIBUTING.md).

## What is not here

- `psarc/`, `widgets/`, `music/`: data taken from the game (archive contents, UI widgets, menu music).
- Decompiler output and `local-config.cmake` (machine-local).

The archive keystream key, nonce and verify key in `CMakeLists.txt` are zero. Set your own with
`-DTO1_PSARC_KEY=... -DTO1_PSARC_NONCE=... -DTO1_VERIFY_KEY=...`.

## Port configuration

The source-mod build exposes compile-time switches that replace the legacy patch-time knobs:

- `TO1_SLOTS4` (default off): build a 4-player client instead of 8.
- `TO1_MM_IP_A`..`TO1_MM_IP_D`, `TO1_MM_PORT`: retarget the matchmaker (was `tools/patch_winmm.py`).
- `TO1_P1_SIDE_SIZE2`, `TO1_P1_SIDE_ID`: the Part I side file (`part1.dat`) size and build id (was
  `tools/features/p1_side_map.py`); `TO1_P1_SIDE_SIZE2 = 0` (default) disables it.


## Building

Requirements:

- Windows, CMake 3.25 or later, Python 3.
- Visual Studio 2022 or 2026 with the "C++ Clang tools for Windows" component. Or an LLVM toolchain on PATH:
  `clang-cl`, `lld-link`, `llvm-rc`, `llvm-ml`, `llvm-lib`, `llvm-strip`.
- `gui2.exe` (widget baker) and `ndarc.exe` (archive packer) on PATH.
- Your own `psarc/` and `widgets/` folders next to `CMakeLists.txt`. The configure step stops when they are empty.

Steps:

1. Copy `local-config.cmake.example` to `local-config.cmake` and set your game folders.
2. Configure and build:

```bat
cmake --preset ninja-release
cmake --build --preset ninja-release
```

Or use the Visual Studio presets (`vs2022`, `to1-release`). `TO1Debug` adds logging and the NET debug menu (F1).
The build fetches safetyhook (with Zydis) from GitHub.
