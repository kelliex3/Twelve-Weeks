"""Build the features winmm.dll.

  python build_dll.py <out.dll> [--port N]

1. mod content: re/modfs (unpacked mod blob, see t2r_pack.py) + overlay files from
   re/dccgame/mods/*/bin/dc1 (scripts compiled with dcc) -> modules.bin sizes patched
   (dc_modules.py) -> packed into the DLL.
2. code patches (winmm_patches.py defaults + tab-native-backpack).
3. t2r-ext.dll loader (ext_loader.py).
4. 8-player (4v4) support: new .t2r8 section + patched sites (slots8.py; T2R_SLOTS8=0 = off).
5. server address 127.0.0.1:<port> (patch_winmm.py; 7001 release, 7101 test instances).

Build time: a per-stage profile table is printed at the end and appended to tools/local/build_profile.log.
Expensive deterministic steps are cached by content hash in tools/local/build-cache (tools/buildcache.py: model paks, HUD icons, map
thumbnails, and the long "decode it back" verifications of the carrier / armour paks); the DSAR lz4 chunks are compressed on all cores
and cached too (t2r_pack.py). The output is byte-identical with the caches off: T2R_NO_BUILD_CACHE=1 T2R_PACK_CACHE=0 (prove it
with `cmp`). T2R_CACHE_DEBUG=1 prints each cache-key part (why did a step rebuild?).
"""
import argparse
import glob
import os
import struct
import shutil
import subprocess
import sys
import time

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
sys.path.insert(0, os.path.join(TOOLS, "features"))
import dc_modules  # noqa: E402
import dc_data_patches  # noqa: E402
import menu_scenes  # noqa: E402
import menu_title  # noqa: E402
import mp_snow  # noqa: E402
import sp_maps_build  # noqa: E402
import p1_side  # noqa: E402
import buildprof as _prof  # noqa: E402
import buildcache as _bc  # noqa: E402

BASE_DLL = os.path.join(TOOLS, "local", "winmm-nocave-127.0.0.1-7001.dll")
# T2R_SLIM=1 (default OFF, does not change the normal output): also write <out>.slim + <out>.slim.dat, a client that does not embed
# the files the player already owns (tools/features/slim.py, re/slim-build.md)
SLIM = os.environ.get("T2R_SLIM") == "1"
_SLIM_PLAN = None
MODFS = os.path.join(ROOT, "re", "modfs")
STAGE = os.path.join(ROOT, "re", "modfs_feat")
OVERLAYS = os.path.join(ROOT, "re", "dccgame", "mods")
# UC4 downed animations (re/uc4ps4, DOWNED.md): the game's anim-san-infected.pak (not loaded in MP otherwise,
# 726 KB) with its 6 clips overwritten by retargeted UC4 clips, plus (mode 1) anim-npc-m-horse.pak with 23 pooled
# carriers (turns, rotations, directional knock-downs, downed deaths), listed in the MP level. Off unless T2R_UC4_DOWNED=1
# (the ext side needs t2r-uc4-downed.txt next to the exe too). T2R_UC4_DOWNED=2 = test build: ONLY downed-idle goes
# into clkr-leash-unaware-idle (play it with the ext dev file t2r-uc4-test.txt, re/uc4ps4/DOWNED.md section 8b).
UC4_MODE = int(os.environ.get("T2R_UC4_DOWNED") or 0)
UC4_DOWNED = UC4_MODE in (1, 2)
UC4_PAK = "anim-san-infected"
UC4_HORSE_PAK = "anim-npc-m-horse"      # mode 1: second carrier pak (SP horse riders, not loaded in MP otherwise)
UC4_PLAYER_PAK = "anim-ellie-workbench-revolver"   # mode 1: own-body carriers baked on the player rig (ellie-skel)
# 2013 gun executions (re/factions2013/gun-executions-plan.md): the game's anim-npc-alert-caps.pak (SP search clips,
# base-female rig, in no MP level) with 24 carrier clips overwritten by the baked TLOU1 execution halves. Listed in the
# MP level; the ext (gunexec_try) plays them when they are loaded. T2R_GUNEXEC=0 = off (default on). If the clips or
# the game pak are missing the build goes on without them (the ext falls back to the plain lethal shot).
GUNEXEC_PAK = "anim-npc-alert-caps"
GUNEXEC_PAK2 = "anim-npc-m-cover-share"      # second carrier pak (build_gunexec_pak.PAK_SETS): the pairs the first one cannot hold
GUNEXEC_PAK3 = "anim-npc-m-lg"              # third carrier pak (EXPLICIT carriers, build_gunexec_pak.THIRD_SET: 2013 ground shiv + the rifle pack executions): the SP NPC pak that is
                                            # already in the MP level and that the Frontier lever stages too (stage_frontier starts from this set's output, p1_carrier exclude=)
GUNEXEC_PAK4 = "anim-npc-m-p"               # fourth carrier pak (POOLED explicit carriers, build_gunexec_pak.FOURTH_SET: the per-gun attacker halves of the Full-Auto / Assault Rifle)
GUNEXEC_PAK5 = "anim-npc-m-lg-enemy-only"   # fifth carrier pak (build_gunexec_pak.FIFTH_SET): the pairs whose victim half plays at 30 fps (SP NPC pak, 5.3 MB, in no MP level: added by GUNEXEC_PAKS)
GUNEXEC_PAKS = [GUNEXEC_PAK, GUNEXEC_PAK2, GUNEXEC_PAK3, GUNEXEC_PAK4, GUNEXEC_PAK5]
_GX_BUILT = {}                              # pak name -> (bytes, res) of build_gunexec_pak.build (the Frontier step needs the third set before stage_gunexec runs)
_GX_FRONTIER_STAGED = []                    # pak names the Frontier step already wrote to the stage on top of the gun execution clips
GUNEXEC = os.environ.get("T2R_GUNEXEC", "1") != "0"
# the weapon's own animation in the 2013 executions (revolver cylinder, tools/models/gx_weapon_clips.py, re/factions2013/exec-weapon-anims.md): T2R_GX_WEAPON=0 off
GX_WEAPON = os.environ.get("T2R_GX_WEAPON", "1") != "0"
if GUNEXEC and not (os.path.exists(os.path.join(ROOT, "re", "factions2013", "t1anim", "baked", "stub.clip")) and
                    (os.path.exists(os.path.join(ROOT, "re", "gamebin", "actor97", GUNEXEC_PAK + ".pak")) or
                     os.path.isdir(os.environ.get("T2_GAME", "C:/Games/The Last of Us Part II Remastered")))):
    print("WARNING: gun execution clips / game not found - building without gun executions")
    GUNEXEC = False
# 2013 Enforcer = the Lost Legacy's Desert Eagle (tools/models/deagle.py, dc_data_patches.DEAGLE_ACTOR).
# T2R_DEAGLE=0: Dina's CZ75 as before; T2R_DEAGLE_TEX=0: Deagle geometry on the CZ75's textures.
DEAGLE = os.environ.get("T2R_DEAGLE", "1") != "0"
DEAGLE_TEX = os.environ.get("T2R_DEAGLE_TEX", "1") != "0"
if not DEAGLE:
    dc_data_patches.CAN_GRAFTS[dc_data_patches.ENFORCER] = dc_data_patches.ENFORCER_CZ_GRAFT
    dc_data_patches.MP_LEVEL_ADDS.remove("actor " + dc_data_patches.DEAGLE_ACTOR)


_ENV_SKIP = ("T2R_MAIN_ROOT", "T2R_SALT0", "T2R_CACHE_DEBUG", "T2R_NO_BUILD_CACHE", "T2R_PACK_CACHE", "T2R_PACK_THREADS", "T2R_NO_NATIVE_FNV",
             "T2R_EXT_READY_FILE", "T2R_SLIM_CHECK_DEFER",      # (build_release handshakes: not inputs of any step)
             "T2R_SLIM_TMP", "T2R_SLIM_CACHE", "T2R_SLIM_ZSTD", "T2R_SLIM_GCCDIR", "T2R_SLIM_DELTA_JOBS", "T2R_SLIM_XZ_THREADS",
             "T2R_SLIM_XZ_BLOCK_MB", "T2R_FAST_PACK", "T2R_LZ4_LEVEL", "T2R_DEV_PACK_CACHE", "T2R_PAR_JOBS")      # (T2R_FAST_PACK / LZ4 level: pack + slim only, after every cached step) (slim step only: it runs after every cached step and has its own content-hash caches)
_env_memo = []


def _env():
    """Inputs common to every cached step, beyond its own code and files (the step modules read these through helpers all over):
    every T2R_* switch plus T2_GAME / UC4_ELF / T2_P1_GAME-style locations, the game installs' archives (TLOU2 and Part I
    build/pc/main by path + size + mtime), the UC4 ELF the animation emulator runs, the emulator / carrier data trees, the pak
    editor binary, and tools/features/data (Frontier clips and events, the contributed files) by content.  Not the checkout location or the
    defender salt.  Computed once per run."""
    if not _env_memo:
        t2 = os.environ.get("T2_GAME", "C:/Games/The Last of Us Part II Remastered")
        sw = {k: v for k, v in os.environ.items() if (k.startswith("T2R_") or k in ("T2_GAME", "UC4_ELF")) and k not in _ENV_SKIP}
        parts = [sw, _bc.Stat(os.path.join(t2, "build", "pc", "main")), _bc.Stat(os.environ.get("UC4_ELF", "E:/Games/WP/uc4/uroot/big4-mp.elf")),
                 _bc.Stat(os.path.join(ROOT, "re", "uc4ps4")), _bc.Stat(os.path.join(ROOT, "re", "factions2013", "t1anim")),
                 _bc.File(os.path.join(ROOT, "tlou2_pak_editor", "build", "gcc", "pak_editor_cli.exe")),
                 _bc.Hashed(os.path.join(TOOLS, "features", "data")), _bc.Hashed(os.path.join(TOOLS, "features", "assets"))]
        try:
            sys.path.insert(0, os.path.join(TOOLS, "part1"))
            import p1_guns
            g = p1_guns.game_dir()
            parts += [g, _bc.Stat(os.path.join(g, "build", "pc", "main"))]      # the Part I install
        except (SystemExit, Exception):
            pass
        _env_memo.append(parts)
    return _env_memo[0]


def _env_scoped(*mods, data=False):
    """_env() narrowed to what a step's code can read: only the T2R_* switches whose names appear in the source of the step's module
    closure (static scan, buildcache.env_names), and the features/data + assets trees only when `data` is true. A step that never reads
    T2R_SP_MAPS / T2R_AUTO_VIS_MAPS / ... is no longer rebuilt because a night build set them differently. The step's own make() in
    this file must not read other switches (checked by hand per call site)."""
    names = _bc.env_names(*mods) | {"T2_GAME", "UC4_ELF"}
    parts = list(_env())
    out = []
    for x in parts:
        if isinstance(x, dict):
            out.append({k: v for k, v in x.items() if k in names})
        elif not data and isinstance(x, _bc.Hashed) and str(x).replace("\\", "/").endswith(("features/data", "features/assets")):
            continue
        else:
            out.append(x)
    return out


def _snow_inputs():
    """what the winter / snow garment builds read beside the game's paks: the reference paks in re/models (this checkout's and the main
    checkout's: armour_paks._load_ref), and the colour-texture directories (mp_snow_pak.TEX_DIRS) - the step keys missed these before"""
    sys.path.insert(0, os.path.join(TOOLS, "models"))
    import armour_paks
    import mp_snow_pak
    out = [_models(), _bc.Content(os.path.join(armour_paks.MAIN, "re", "models"), "*.pak") if os.path.isdir(os.path.join(armour_paks.MAIN, "re", "models")) else "no-main-models"]
    out += [_bc.Hashed(d) if os.path.isdir(d) else "no-tex-dir" for d in mp_snow_pak.TEX_DIRS if d]
    return out


def _re(*p):
    return _bc.Stat(os.path.join(ROOT, "re", *p))


def _models():
    """re/models' top-level paks by content (previews / per-step cache directories next to them change on every build)"""
    return _bc.Content(os.path.join(ROOT, "re", "models"), "*.pak")


def _verify_cached(name, mods, artifacts, fn, stats=(), extra=()):
    """a pure verification (the step's pak decoded back / compared) that passed for these exact artifact bytes + this code is not
    repeated (tools/buildcache.py; T2R_NO_BUILD_CACHE=1 runs it every time)"""
    import importlib.util
    return _bc.verified(name, [_bc.Code(*mods), _env_scoped(*mods, data=not name.startswith("gunexec")), importlib.util.find_spec("capstone") is not None,
                               [_bc.File(a) if isinstance(a, str) else a for a in artifacts], [_bc.Stat(x) for x in stats], extra], fn)


def _p1_inputs():
    """what the Part I model builds read besides code: the extracted Part I data in the main checkout and the install's archives"""
    sys.path.insert(0, os.path.join(TOOLS, "part1"))
    import p1_guns
    parts = [_bc.Stat(p1_guns.P1DIR), _models()]
    try:
        g = p1_guns.game_dir()
        parts += [g, _bc.Stat(os.path.join(g, "build", "pc", "main"))]
    except SystemExit:
        pass                      # (the build itself stops with the clear message)
    return parts


def _cached_pak(name, mods, out, make, inputs=()):
    """a model pak built by `make()` into `out`, rebuilt only when its code / inputs / T2R_* switches changed"""
    def run():
        r = make()
        assert r in (None, out), (name, r)
    _bc.cached(name, [_bc.Code(*mods), _env_scoped(*mods), list(inputs)], run, files=[out])
    return out


def _colt_work_pak():
    """the Part I Colt Defender built into a copy of the Beretta's pak (tools/part1/p1_guns.COLT), once per build: the
    HUD icons render it, the suppressor-can graft starts from it"""
    sys.path.insert(0, os.path.join(TOOLS, "part1"))
    import p1_guns
    path = os.path.join(STAGE, "actor97", "colt-work.pak")
    if not os.path.exists(path):
        _cached_pak("model-colt", ["p1_guns"], path, lambda: p1_guns.build_colt(path), _p1_inputs())
    return path


def stage_modfs(salt=0):
    if os.path.exists(STAGE):
        shutil.rmtree(STAGE)
    shutil.copytree(MODFS, STAGE)
    _prof.lap('staging: rmtree + copytree of re/modfs')
    dc_data_patches.write_faction_guns_header()        # t2r-ext faction_guns_on() follows this build (not the txt)
    mp_snow.write_header()                             # t2r-ext mpsnow.c drives the snow tables only when this build carries them (T2R_MP_SNOW=1)
    man = os.path.join(STAGE, "_t2r_manifest.txt")
    lines = open(man).read().split("\n")
    k = lines.index("--order--")
    paths, order = lines[1:k], lines[k + 1:]
    modules = os.path.join(STAGE, "bin", "dc1", "modules.bin")
    data = bytearray(open(modules, "rb").read())
    dc_data_patches.make_crafting_overlay(os.path.join(ROOT, "re", "gamebin", "dc1", "crafting-system.bin"),
                                          os.path.join(OVERLAYS, "t2r-data", "bin", "dc1"))
    # attach-points.bin (supplied): fixes where arrows and melee weapons sit on NPC
    # (puppet) backpacks. Same size as the base game file; shipped as a whole-file overlay.
    dc_data_patches.make_explosions_overlay(os.path.join(ROOT, "re", "gamebin", "dc1", "explosions.bin"),
                                            os.path.join(OVERLAYS, "t2r-data", "bin", "dc1"))
    dc_data_patches.make_difficulty_overlay(os.path.join(ROOT, "re", "gamebin", "dc1", "difficulty.bin"),
                                            os.path.join(OVERLAYS, "t2r-data", "bin", "dc1"))
    dc_data_patches.make_bullet_overlay(os.path.join(ROOT, "re", "gamebin", "dc1", "bullet-settings.bin"),
                                        os.path.join(OVERLAYS, "t2r-data", "bin", "dc1"))
    dc_data_patches.make_finisher_overlays(os.path.join(ROOT, "re", "gamebin", "dc1", "melee", "player-standing"),
                                           os.path.join(OVERLAYS, "t2r-data", "bin", "dc1", "melee", "player-standing"),
                                           os.path.join(TOOLS, "win", "ext", "dcmain", "build", "dconstruct.exe"),
                                           os.path.join(ROOT, "tools", "local"))
    # melee pull range: the "-far" lunge moves start from 2.0 m (was 3.0 m: a 2-3 m gap with no pulling move = whiff in place);
    # T2R_MELEE_FAR_MIN=3.0 = the game's gate. re/melee-pull-range.md
    _mfm = float(os.environ.get("T2R_MELEE_FAR_MIN", dc_data_patches.MELEE_FAR_MIN_NEW))
    _mdir = os.path.join(ROOT, "re", "gamebin", "dc1", "melee", "player-standing")
    _modir = os.path.join(OVERLAYS, "t2r-data", "bin", "dc1", "melee", "player-standing")
    if dc_data_patches.patch_melee_far_range(_mdir, _modir, _mfm):
        dc_data_patches.verify_melee_far_range(_mdir, _modir, _mfm)
    # 9mm hammer click: the Beretta's hammer events under the 1911 audio set's names (COLT_AUDIO set has none)
    dc_data_patches.make_colt_hammer_overlay(os.path.join(ROOT, "re", "gamebin", "dc1"),
                                             os.path.join(OVERLAYS, "t2r-data", "bin", "dc1"))
    dc_data_patches.make_tac_sfx_overlay(os.path.join(ROOT, "re", "gamebin", "dc1"),
                                         os.path.join(OVERLAYS, "t2r-data", "bin", "dc1"))
    # remote players' footsteps / gear at 2013-like range (mix-default foley-npc rows; T2R_NPC_FOLEY_MIX=0 = base mix)
    dc_data_patches.make_audio_mix_overlay(os.path.join(ROOT, "re", "gamebin", "dc1"),
                                           os.path.join(OVERLAYS, "t2r-data", "bin", "dc1"))
    # mesh-audio-config.bin: the Rattler helmet part (in every armoured look twin) is flagged "gas mask" there -> every voice futzed
    dc_data_patches.make_mesh_audio_overlay(os.path.join(ROOT, "re", "gamebin", "dc1"),
                                            os.path.join(OVERLAYS, "t2r-data", "bin", "dc1"))
    # execution gun recoil: six gesture defs of gestures/ellie-gestures.bin re-pointed at TLOU2's own additive fire clips (same file size);
    # the ext plays them at each execution shot (t2r-no-exec-recoil.txt = off at run time). T2R_EXEC_RECOIL=0 = the game's file (no overlay)
    import exec_recoil_gestures
    _erg = os.path.join(OVERLAYS, "t2r-data", "bin", "dc1", "gestures", "ellie-gestures.bin")
    if os.environ.get("T2R_EXEC_RECOIL", "1") == "0":
        if os.path.exists(_erg):
            os.remove(_erg)
    else:
        exec_recoil_gestures.make_overlay(os.path.join(ROOT, "re", "gamebin", "dc1"), os.path.join(OVERLAYS, "t2r-data", "bin", "dc1"))
    # vox/characters (+ vox-rl): the SP player's effort lines for the MP voices (SP parity; T2R_SP_VOX_EFFORTS=0 = off)
    dc_data_patches.make_vox_effort_overlay(os.path.join(ROOT, "re", "gamebin", "dc1"),
                                            os.path.join(OVERLAYS, "t2r-data", "bin", "dc1"))
    # props.bin: definitions for the armoured twins' pushed cloth-prop copies (without one a look2 prop spawns
    # nothing - coat missing); removed again when the copies are off (T2R_ARMOUR_PROP_PUSH=0 / T2R_NO_ARMOUR_UNDER=1; on by default)
    dc_data_patches.make_props_overlay(os.path.join(ROOT, "re", "gamebin", "dc1", "props.bin"),
                                       os.path.join(OVERLAYS, "t2r-data", "bin", "dc1"))
    # weapon-straps.bin: rifle sling offsets (slot one + two) for every MP look and armour twin (re/weapon-straps.md); T2R_STRAPS=0 = game file
    import weapon_straps
    ws = os.path.join(OVERLAYS, "t2r-data", "bin", "dc1", "weapon-straps.bin")
    if os.environ.get("T2R_STRAPS", "1") == "0":
        if os.path.exists(ws):
            os.remove(ws)
    else:
        weapon_straps.make(os.path.join(ROOT, "re", "gamebin", "dc1", "weapon-straps.bin"), os.path.dirname(ws))
    # menu music: the five 2013 lobby tracks as five cues of the credits-in-menu playlist (random order); T2R_MENU_MUSIC=0 = the
    # joined 7:32 cue as one chunk (the stock cue, tracks always in order from #1)
    mm = os.path.join(OVERLAYS, "t2r-data", "bin", "dc1", "music-settings.bin")
    if os.environ.get("T2R_MENU_MUSIC", "1") == "0":
        if os.path.exists(mm):
            os.remove(mm)
    else:
        dc_data_patches.make_menu_music_overlay(os.path.join(ROOT, "re", "gamebin", "dc1"), os.path.dirname(mm))
    # title (boat) screen background: Hillcrest overlook instead of the boat (tools/features/menu_title.py, re/menu-title-hillcrest.md); the
    # whole-file override of level-sets-menu.bin ships inside the mod, so the launcher's SP switch (mod parked) keeps the boat. T2R_MENU_BOAT=1 = the boat
    mt = os.path.join(OVERLAYS, "t2r-data", "bin", "dc1", "level-sets", "level-sets-menu.bin")
    if os.environ.get("T2R_MENU_BOAT", "0") == "1":
        if os.path.exists(mt):
            os.remove(mt)
    else:
        menu_title.make_overlay(os.path.join(ROOT, "re", "gamebin", "dc1", "level-sets", "level-sets-menu.bin"), os.path.dirname(mt))
    msp = os.path.join(OVERLAYS, "t2r-data", "bin", "dc1", "material-param.bin")
    if mp_snow.ON:      # MP snow (prototype, T2R_MP_SNOW=1): two SP snow tables re-pointed at the snow garment(s) of mp_snow.PAKS
        mp_snow.make_overlay(os.path.join(ROOT, "re", "gamebin", "dc1", "material-param.bin"), os.path.dirname(msp))
    elif os.path.exists(msp):
        os.remove(msp)
    _prof.lap('data overlays (crafting/explosions/difficulty/bullet/finisher/tac sfx/mix/props/straps)')
    shutil.copyfile(os.path.join(TOOLS, "features", "data", "attach-points.bin"),
                    os.path.join(OVERLAYS, "t2r-data", "bin", "dc1", "attach-points.bin"))
    # The supplied Edits.zip: files we never edited go in as supplied; the three we already edit are merged
    # (attach-points.bin 3-way into data/attach-points.bin, weapon-art.bin replayed by name below, look2/characters.bin =
    # the supplied edited copy of our base in re/modfs). T2R_NO_WEDGE=1 builds without the supplied files (A/B comparison).
    wedge = os.path.join(TOOLS, "features", "data", "wedge")
    for src in glob.glob(os.path.join(wedge, "**", "*.bin"), recursive=True):
        rel = os.path.relpath(src, wedge).replace("\\", "/")
        if rel in ("weapon-art.bin", "attach-points.bin", "look2/characters.bin"):
            continue
        dst = os.path.join(OVERLAYS, "t2r-data", "bin", "dc1", *rel.split("/"))
        if os.environ.get("T2R_NO_WEDGE"):
            if os.path.exists(dst):
                os.remove(dst)
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        print(f"wedge: {rel}")
    _prof.lap('wedge files copy')
    for dc1 in glob.glob(os.path.join(OVERLAYS, "*", "bin", "dc1")):
        for src in glob.glob(os.path.join(dc1, "**", "*.bin"), recursive=True):
            rel = os.path.relpath(src, dc1).replace("\\", "/")
            if rel == "modules.bin":
                continue
            p = "bin/dc1/" + rel
            dst = os.path.join(STAGE, p)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy(src, dst)
            if p not in paths:          # keep new scripts next to the mod's other bin/dc1 files
                for lst in (paths, order):
                    last = max(i for i, x in enumerate(lst) if x.startswith("bin/dc1/"))
                    lst.insert(last + 1, p)
            try:
                old = dc_modules.set_size(data, rel[:-4], os.path.getsize(src))
            except SystemExit:          # not a module (per-weapon overlay sets are loaded by path): no size entry to fix
                old = "no module entry"
            print(f"overlay {p} ({old} -> {os.path.getsize(src)} bytes)")
    open(modules, "wb").write(data)
    if os.path.exists(mm):          # the five lobby-track cues (tools/features/menu_music.py): music1 files of the donor playlist's first five cues
        import menu_music
        if menu_music.have_ps4(ROOT):          # the game's own PS4 streams (no buzz); the MP3-rip slices only when they were not extracted
            cues = menu_music.make_cues_ps4(ROOT, os.path.join(STAGE, "music1"))
        else:
            cues = menu_music.make_cues(os.path.join(STAGE, "music1", "022_t2r_credits_in_menu.xvag"), os.path.join(STAGE, "music1"))
        for name, size in cues:
            print(f"menu music: music1/{name} ({size} bytes)")
            if "music1/" + name not in paths:
                for lst in (paths, order):
                    last = max(i for i, x in enumerate(lst) if x.startswith("music1/"))
                    lst.insert(last + 1, "music1/" + name)
    stage_holster_draw(paths, order)
    # skinned armour (re/armor-rig.md): armoured twins "<look>-t2r-arm" of the pool looks in the mod's
    # look2/characters.bin (+ its modules.bin exports / size); t2r-ext spawns players with them
    _prof.lap('overlay copy into stage + modules.bin sizes')
    chars = os.path.join(STAGE, "bin", "dc1", "look2", "characters.bin")
    # part swaps first (the twins copy the base's parts): player-style rigged backpacks
    # (re/backpack-rig.md) + scar-male-01's head; then the twins; then the Seraphite skin tint
    # palettes get entries for the twins (re/faction-looks.md "Same face")
    dc_data_patches.patch_look_parts(chars, modules)
    dc_data_patches.add_scar_holsters(chars)        # Seraphite back-of-belt holster part (before the twins: they copy the base's parts)
    dc_data_patches.add_second_holsters(chars)      # second-sidearm holster part per look, hidden at spawn (tools/models/second_holster.py)
    dc_data_patches.verify_armour_looks(chars, dc_data_patches.patch_armour_looks(chars, modules))
    dc_data_patches.patch_look_tints(chars, modules)
    if dc_data_patches.WINTER_ON:       # T2R_MP_SNOW=1 + T2R_FACTION_LOOKS=1: the winter crew looks (tools/models/crew_winter.py; the ext wears them on winter maps)
        dc_data_patches.patch_winter_looks(chars, modules)
        dc_data_patches.verify_winter_looks(chars)
        wl = list(dc_data_patches.WINTER_LOOKS)         # their armoured twins (helmet + vest on the winter map), then the skin tints of the Seraphite ones (base look's colour)
        dc_data_patches.verify_armour_looks(chars, dc_data_patches.patch_armour_looks(chars, modules, bases=wl))
        dc_data_patches.patch_look_tints(chars, modules, bases=[l for l in wl if l.startswith("scar-")], alias={l: dc_data_patches.WINTER_LOOKS[l][0] for l in wl})
    # TAB-menu backpack props per look backpack (re/backpack-rig.md "TAB menu prop"): look2 entries
    if dc_data_patches.BACKPACK_PROPS_ON:
        dc_data_patches.verify_backpack_prop_looks(chars, dc_data_patches.patch_backpack_prop_looks(chars, modules))
    dc_data_patches.verify_look_patches(chars)
    _prof.lap('look2 patches (parts, armour twins, tints, backpack props)')
    # more team voice lines (tools/features/vox_lines.py, re/vox-lines.md): vox-info records in the mod's
    # vox-igc-greenlight module + its modules.bin exports; T2R_NO_VOX_MORE=1 = without (winmm's 126 lines only)
    if dc_data_patches.VOX_MORE_ON:
        gl = os.path.join(STAGE, "bin", "dc1", "vox-igc-greenlight.bin")
        dc_data_patches.patch_vox_lines(gl, modules)
        dc_data_patches.verify_vox_lines(gl, modules)
        if os.environ.get("T2R_NO_VOX_BURN") != "1":        # (crash bisect: T2R_NO_VOX_BURN=1 = without them)
            dc_data_patches.patch_vox_burn(gl, modules)     # the burning-death screams, every voice set's own fire-death takes
            dc_data_patches.verify_vox_burn(gl, modules)
            dc_data_patches.patch_vox_beg(gl, modules)      # execution begging (pleas + bullet-hit grunts, every voice set)
            dc_data_patches.verify_vox_beg(gl, modules)
            dc_data_patches.patch_vox_downed(gl, modules)   # downed groans (bleed-out groans + injured breaths, every voice set)
            dc_data_patches.verify_vox_downed(gl, modules)
        if dc_data_patches.FEMALE_ON:       # T2R_FEMALE=1: the female voice sets (re/female-characters.md)
            dc_data_patches.patch_vox_lines_female(gl, modules)
            dc_data_patches.verify_vox_lines_female(gl, modules)
    _prof.lap('vox lines')
    # MP levels edited in TLOUSceneEditor (spawn points): tools/features/levels/*.pak replace
    # the mod's pak68 copies whole (the archive is rebuilt, so sizes may change)
    sp_maps_build.ensure_layouts(sp_maps_build.selected())     # tools/maps/layout.py ensure: new maps get a generated, checked layout (never fails the build)
    sp_on = {m["pak"] + "-ingame.pak" for m in sp_maps_build.selected()}
    retired_paks = {"to1-pvp-hospital-ingame.pak"} if sp_maps_build.hospital_replaced() else set()   # the old NoR hospital's spawner pak (kept in the repo; T2R_HOSPITAL_OLD=1 stages it)
    sp_all = {m["pak"] + "-ingame.pak" for m in sp_maps_build.specs()}
    for src in glob.glob(os.path.join(TOOLS, "features", "levels", "*.pak")):
        if os.path.basename(src) in (sp_all - sp_on) | retired_paks:     # SP-section map not in this build (T2R_SP_MAPS) / the replaced Hospital's pak
            continue
        dst = os.path.join(STAGE, "pak68", os.path.basename(src))
        if not os.path.exists(dst):                 # a new map's pak: also into the manifest
            p = "pak68/" + os.path.basename(src)
            for lst in (paths, order):
                last = max(i for i, x in enumerate(lst) if x.startswith("pak68/"))
                lst.insert(last + 1, p)
        if not os.path.exists(dst) or open(src, "rb").read() != open(dst, "rb").read():
            shutil.copyfile(src, dst)
            print(f"level {os.path.basename(src)} ({os.path.getsize(src)} bytes)")
    _prof.lap('sp_maps ensure_layouts + level pak copies')
    dc_data_patches.patch_hospital_map(STAGE)
    _prof.lap('hospital map patch')
    # faction MP menu backgrounds (menu_scenes.py, re/mp-menu-background.md): WLF stadium / Scar island
    # level lists for the unused starlight menu tasks + their camera paks. T2R_NO_MENU_SCENES=1: off
    # (t2r-ext checks the level set's first level and keeps the default scene on such a build).
    ls_size = None if os.environ.get("T2R_NO_MENU_SCENES") else menu_scenes.apply(STAGE, paths, order)
    if os.path.exists(os.path.join(STAGE, "bin", "dc1", "level-sets", "level-sets-menu.bin")) and menu_title.patch_registry(os.path.join(STAGE, "pak68.txt")):
        print("menu title: pak68.txt menu-beach-script declares sound-bank gbl-main-menu")
    _prof.lap('menu_scenes')
    # PvP maps from single-player sections (tools/maps/sp_maps.json, re/maps/technical.md): T2R_SP_MAPS=hardware,motel
    # or all; off by default. New task subnodes under to1-levels + level lists in leftover dev level sets.
    sp_maps_build.CACHE_PARTS = [_env_scoped("collectibles")]        # (the per-map collectible / workbench strip is a cached step)
    sp_sizes = sp_maps_build.apply(STAGE)
    _prof.lap('sp_maps_build.apply (level sets / Part I paks)')
    # Part I levels (stage_part1 copies their converted paks into pak68/): they must be in the manifest too, or the
    # archive never carries them and the engine has nothing to load (Bookstore gray / fall loop, the three
    # hun-checkpoint-* levels never 'loaded' while our own spawner level was)
    p1_new = 0
    p1_rel = set()          # every Part I-derived archive path of this build (T2R_P1_SIDE=1 moves them out of the blob, below)
    for m in sp_maps_build.selected():
        for lv in sp_maps_build.p1_levels(m) + ([sp_maps_build.p1_rs(m)] if sp_maps_build.p1_rs(m) else []):
            for f in sorted(glob.glob(os.path.join(STAGE, "pak68", lv + "*.pak"))):
                p = "pak68/" + os.path.basename(f)
                p1_rel.add(p)
                for lst in (paths, order):
                    if p not in lst:
                        last = max(i for i, x in enumerate(lst) if x.startswith("pak68/"))
                        lst.insert(last + 1, p)
                        p1_new += lst is paths
        if sp_maps_build.p1_rs(m):          # Part I render settings: the staged render-settings/t1x module (re/part1/geometry-upgrade.md s11)
            p = "bin/dc1/" + sp_maps_build.RS_MODULE + ".bin"
            p1_rel.add(p)
            for lst in (paths, order):
                if p not in lst:
                    last = max(i for i, x in enumerate(lst) if x.startswith("bin/dc1/"))
                    lst.insert(last + 1, p)
                    p1_new += lst is paths
    for m in sp_maps_build.selected():      # Part I Bookstore ambience (p1_audio.py): its 2 level paks, banks, streams and carrier DC modules (staged by sp_maps_build.stage_part1_audio)
        for p in sp_maps_build.p1_audio_files(m) + sp_maps_build.p1_actor_files(m):   # (+ the converted Part I actor paks: Part I map data, test builds only)
            p1_rel.add(p)
            for lst in (paths, order):
                if p not in lst:
                    grp = [i for i, x in enumerate(lst) if x.startswith(p.split("/")[0] + "/")] or                           [i for i, x in enumerate(lst) if x.startswith(("soundbank4/", "gtex1/", "pak68/"))]
                    lst.insert((max(grp) + 1) if grp else len(lst), p)
                    p1_new += lst is paths
    for m in sp_maps_build.selected():      # The fire barrels' sound level (p1_firesfx.py): donor-derived TLOU2 data, in the blob in every build (not p1_rel)
        for p in sp_maps_build.p1_fire_sfx_files(m):
            for lst in (paths, order):
                if p not in lst:
                    last = max(i for i, x in enumerate(lst) if x.startswith("pak68/"))
                    lst.insert(last + 1, p)
    if p1_new:
        print(f"sp maps: {p1_new} Part I level paks / audio files added to the manifest")
    # T2R_P1_SIDE=1 (tools/features/p1_side.py, re/part1/side-files.md): the Part I files leave the blob for a side tree + manifest (the launcher
    # puts them at <game dir>/t2r-maps/part1/); the blob keeps only the registry entries (pak68.txt blocks, modules.bin, our spawner pak).
    # The generated ext header always says what this build expects (side off: T2R_P1_SIDE_BUILT 0, the ext guard is inert).
    side_res = None
    global _P1_VIRT
    _P1_VIRT = None
    if p1_side.virt() and p1_rel:
        # virtual-region mode (default of T2R_P1_SIDE=1): the files stay in the stage; build() cuts their data off the archive (t2r_pack.pack_split_bytes) and
        # ships it as part1.dat, which the winmm .t2rps cave maps behind the blob. The ext learns the outcome from the cave's descriptor, so its header
        # carries no pack-dependent value.
        _side, _kept = p1_side.split(p1_rel, modules=False)      # (DC modules stay: modules.bin registers them at boot)
        _entries = p1_side.audit_entries(STAGE, _side)
        _P1_VIRT = {"paths": _side, "kept": _kept, "entries": _entries}
        mask = 0
        for m in sp_maps_build.selected():
            if sp_maps_build.p1_levels(m):
                mask |= sp_maps_build.built_mask([m])
        p1_side.write_ext_header(os.path.join(TOOLS, "ext", "p1side_built.h"), None, len(_entries), sum(e[1] for e in _entries), mask, virt=True)
        print(f"p1 side (virtual region): {len(_side)} Part I files ({sum(e[1] for e in _entries) / 1e6:.1f} MB) will be cut off the archive into part1.dat; {len(_kept)} DC modules stay")
    elif p1_side.enabled() and p1_rel:
        side_res = p1_side.extract(STAGE, p1_rel, paths, order, p1_side.out_dir(ROOT))
        bad = p1_side.verify_blob(paths, order, side_res["entries"], STAGE)
        assert not bad, f"p1 side: Part I files still in the blob: {bad[:5]}"
        mask = 0
        for m in sp_maps_build.selected():
            if sp_maps_build.p1_levels(m):
                mask |= sp_maps_build.built_mask([m])
        p1_side.write_ext_header(os.path.join(TOOLS, "ext", "p1side_built.h"), side_res["crc"], len(side_res["entries"]), side_res["bytes"], mask)
    else:
        p1_side.write_ext_header(os.path.join(TOOLS, "ext", "p1side_built.h"))
    # edited stock SP levels (sp_maps.json "edits", tools/maps/level_edit.py): same names as the stock paks, shipped from the mod tree
    for p in (sp_sizes or {}).get("edit-paks", []):
        for lst in (paths, order):
            if p not in lst:
                last = max(i for i, x in enumerate(lst) if x.startswith(p.split("/")[0] + "/"))
                lst.insert(last + 1, p)
        print(f"sp maps: edited level pak {p} added to the manifest")
    tt_size = None
    if sp_sizes:
        ls_size, tt_size = sp_sizes["level-sets"], sp_sizes["test-tasks"]
    _prof.lap('part I level manifest')
    dc_data_patches.patch_weapon_damages(os.path.join(STAGE, "bin", "dc1", "weapon-damages.bin"))
    dc_data_patches.patch_molotov_damage(os.path.join(STAGE, "bin", "dc1", "weapon-damages.bin"))   # 2013: no impact damage
    # The hard-landing tier for the Bookstore atrium drop (the landing records of player-settings.bin; an overlay copy of the stock game file, size unchanged).
    # T2R_LANDING_PATCH=0 = off.
    if os.environ.get("T2R_LANDING_PATCH", "1") != "0":
        dc_data_patches.make_landing_overlay(os.path.join(ROOT, "re", "gamebin", "dc1", "player-settings.bin"),
                                             os.path.join(OVERLAYS, "t2r-data", "bin", "dc1"))
    dc_data_patches.patch_weapon_cross(os.path.join(STAGE, "bin", "dc1", "player-weapon-cross.bin"))
    dc_data_patches.patch_camera_body_scale(os.path.join(STAGE, "bin", "dc1", "camera.bin"))   # MP bodies vs Ellie / Abby framing
    dc_data_patches.patch_pace_motion_matching(os.path.join(STAGE, "bin", "dc1", "motion-matching-settings"))   # PACE_2013 (T2R_PACE_2013=0 = off)
    wu = os.path.join(STAGE, "bin", "dc1", "weapon-upgrades.bin")
    dc_data_patches.patch_weapon_upgrades(wu)
    dc_data_patches.add_melee_upgrade_bundles(wu)       # the MP tomahawk could not take the melee upgrade (no bundle)
    dc_data_patches.patch_player_inventory(os.path.join(STAGE, "bin", "dc1", "player-inventory.bin"))   # 2013 max reserve per upgrade level
    dc_data_patches.patch_satchel_item_name(os.path.join(STAGE, "bin", "dc1", "player-inventory.bin"))   # the supply satchel pickup (parts item) reads 'Supply Satchel'
    if os.environ.get("T2R_GIFT", "1") != "0":
        dc_data_patches.add_gift_item(os.path.join(STAGE, "bin", "dc1", "player-inventory.bin"))      # Crafter gift item (gift.c)
    mdata = bytearray(open(modules, "rb").read())        # the file grew: modules.bin lists sizes
    dc_modules.set_size(mdata, "weapon-upgrades", os.path.getsize(wu))
    dc_modules.set_size(mdata, "player-inventory", os.path.getsize(os.path.join(STAGE, "bin", "dc1", "player-inventory.bin")))   # the satchel icon repoint (T2R_SATCHEL_ICON) appends a string: modules.bin must list the new size (the missing entry = a startup crash, exe+14bf383, suspected)
    if ls_size:
        dc_modules.set_size(mdata, "level-sets/level-sets-test", ls_size)
    if tt_size:
        dc_modules.set_size(mdata, "test-tasks", tt_size)
    # (before the pak68 patch: its carrier paks are MP level adds)
    DB_RELOAD_ADDS = stage_db_reload(paths, order, os.environ.get("T2R_DB_RELOAD_WARP", "1") != "0")
    _prof.lap('stage_db_reload (re-timed Double Barrel reload clips)')
    dc_data_patches.patch_pak68(os.path.join(STAGE, "pak68.txt"),
                                adds=dc_data_patches.MP_LEVEL_ADDS + (["actor " + UC4_PAK] if UC4_DOWNED else []) +
                                     (["actor " + UC4_HORSE_PAK, "actor " + UC4_PLAYER_PAK] if UC4_MODE == 1 else []) +
                                     (["actor " + n for n in GUNEXEC_PAKS] if GUNEXEC else []) +
                                     (["actor anim-ellie-horse"] if os.environ.get("T2R_EXEC_RECOIL", "1") != "0" else []) + DB_RELOAD_ADDS)
    # Full Auto Rifle shot: the MPX5's own sound bank with the Ruger's waves inside (sndbank.py;
    # pointing the gun at the Ruger's sound event broke full-auto fire)
    _prof.lap('weapon bin patches + pak68.txt')
    import sndbank
    far_orig = open(os.path.join(ROOT, "re", "sound", "wpn-rifle-autorifle4.bnk"), "rb").read()
    far = sndbank.replace_waves(far_orig,
                                open(os.path.join(ROOT, "re", "sound", "wpn-pistol-storm.bnk"), "rb").read(),
                                dc_data_patches.FAR_WAVES)
    if dc_data_patches.FAR_PCM:
        far = sndbank.replace_pcm(far, os.path.join(ROOT, "re", "sound", "wav", "wpn-rifle-autorifle4"), dc_data_patches.FAR_PCM)
    os.makedirs(os.path.join(STAGE, "soundbank4"), exist_ok=True)
    open(os.path.join(STAGE, "soundbank4", "wpn-rifle-autorifle4.bnk"), "wb").write(far)
    for lst in (paths, order):
        if "soundbank4/wpn-rifle-autorifle4.bnk" not in lst:
            last = max(i for i, x in enumerate(lst) if x.startswith("gtex1/"))
            lst.insert(last + 1, "soundbank4/wpn-rifle-autorifle4.bnk")
    if dc_data_patches.FAR_NPC_SHOT:
        print("sound: Full Auto Rifle = the MPX5's unsuppressed npc shot with the Ruger's stereo blooms; the suppressed event stays the game's (MPX-S)")
    else:
        print("sound: Full Auto Rifle = unsuppressed MPX5 (Ruger tail, unsuppressed thumps) on the suppressed event (T2R_FAR_NPC_SHOT=0)")
    # suppressed pistols: bloom = Beretta's suppressed bloom-quad-v1-1, bass = MPX5 suppressed thumps
    beretta = open(os.path.join(ROOT, "re", "sound", "wpn-pistol-a-semi-auto.bnk"), "rb").read()
    for bank in dc_data_patches.PISTOL_SUPP_BANKS:
        data = open(os.path.join(ROOT, "re", "sound", bank + ".bnk"), "rb").read()
        data = sndbank.replace_waves(data, beretta, dc_data_patches.PISTOL_SUPP_WAVES)
        data = sndbank.replace_pcm(data, os.path.join(ROOT, "re", "sound", "wav", bank), dc_data_patches.PISTOL_SUPP_PCM,
                                   os.path.join(ROOT, "re", "sound", "wav", "wpn-rifle-autorifle4"))
        open(os.path.join(STAGE, "soundbank4", bank + ".bnk"), "wb").write(data)
        for lst in (paths, order):
            if f"soundbank4/{bank}.bnk" not in lst:
                last = max(i for i, x in enumerate(lst) if x.startswith("gtex1/") or x.startswith("soundbank4/"))
                lst.insert(last + 1, f"soundbank4/{bank}.bnk")
    print("sound: suppressed pistols = Beretta suppressed bloom + MPX5 suppressed thumps")
    # the purchasable Galil keeps its stock shot: the game's bank, byte for byte (the suppressed rifles fire the MPX5's SP suppressed
    # event, SUPP_RIFLE_*; they used to share the Galil's event, with its waves edited)
    galil_orig = open(os.path.join(ROOT, "re", "sound", dc_data_patches.RIFLE_SUPP_BANK + ".bnk"), "rb").read()
    galil = galil_orig
    open(os.path.join(STAGE, "soundbank4", dc_data_patches.RIFLE_SUPP_BANK + ".bnk"), "wb").write(galil)
    for lst in (paths, order):
        if f"soundbank4/{dc_data_patches.RIFLE_SUPP_BANK}.bnk" not in lst:
            last = max(i for i, x in enumerate(lst) if x.startswith("gtex1/") or x.startswith("soundbank4/"))
            lst.insert(last + 1, f"soundbank4/{dc_data_patches.RIFLE_SUPP_BANK}.bnk")
    if dc_data_patches.SUPP_CARRIER_ON:
        # the suppressed rifles' mix (what the Galil bank used to carry) in the carrier set's bank: Tommy's sniper set, loaded via its actor,
        # event retargeted (dc_data_patches.SUPP_CARRIER_*)
        carrier = dc_data_patches.build_supp_carrier_bank(ROOT)
        cb = dc_data_patches.SUPP_CARRIER_BANK
        open(os.path.join(STAGE, "soundbank4", cb + ".bnk"), "wb").write(carrier)
        for lst in (paths, order):
            if f"soundbank4/{cb}.bnk" not in lst:
                last = max(i for i, x in enumerate(lst) if x.startswith("gtex1/") or x.startswith("soundbank4/"))
                lst.insert(last + 1, f"soundbank4/{cb}.bnk")
        dc_data_patches.verify_supp_carrier(ROOT, os.path.join(ROOT, "re", "gamebin", "dc1"), carrier)
        print(f"sound: Galil = the game's stock bank (stock shot); suppressed rifles = the old mix on {dc_data_patches.SUPP_CARRIER_EVENT} (bank {cb})")
    else:
        print("sound: Galil = the game's stock bank (stock shot); suppressed rifles = the MPX5's SP suppressed shot (T2R_SUPP_CARRIER=0)")
    # Tactical Shotgun shots (not the pump's sound; the suppressed one like the
    # Chigurh "silencedshotgun.wav"): two shotgun banks no MP gun uses, loaded in matches (pak68)
    #   Abby's double barrel (sfx-shotgun-dbl-barrel-abby-player): blooms = the hunting rifle's
    #   Seraphite double barrel (sfx-shotgun-dbl-barrel-scar-npc): thumps = the Chigurh shot
    #   (re/sound/custom, 48 kHz mono), every other shot layer silent (TAC_SILENCE: no tail)
    for bank, waves, pcm in dc_data_patches.TAC_BANKS:
        data = open(os.path.join(ROOT, "re", "sound", bank + ".bnk"), "rb").read()
        for src_bank, mapping in waves:
            data = sndbank.replace_waves(data, open(os.path.join(ROOT, "re", "sound", src_bank + ".bnk"), "rb").read(), mapping)
        if pcm:
            # the Chigurh shot at TAC_THUMP_GAIN + the pump shotgun's LFE, pre-mixed into one wav
            gen = os.path.join(ROOT, "tools", "local", "tac-sound")
            os.makedirs(gen, exist_ok=True)
            lsrc, lwave, lgain = dc_data_patches.TAC_LFE
            for f in set(pcm.values()):
                sndbank.mix_wav_mono(glob.glob(os.path.join(ROOT, "re", "sound", "custom", "*" + f))[0], dc_data_patches.TAC_THUMP_GAIN,
                                     glob.glob(os.path.join(ROOT, "re", "sound", "wav", lsrc, "*_" + lwave))[0], lgain,
                                     os.path.join(gen, f))
            data = sndbank.replace_pcm(data, os.path.join(ROOT, "re", "sound", "wav", bank), pcm, gen)
        if bank in dc_data_patches.TAC_SILENCE:
            data = sndbank.silence_waves(data, dc_data_patches.TAC_SILENCE[bank])
        open(os.path.join(STAGE, "soundbank4", bank + ".bnk"), "wb").write(data)
        for lst in (paths, order):
            if f"soundbank4/{bank}.bnk" not in lst:
                last = max(i for i, x in enumerate(lst) if x.startswith("gtex1/") or x.startswith("soundbank4/"))
                lst.insert(last + 1, f"soundbank4/{bank}.bnk")
        print(f"sound: {bank} rebuilt for the Tactical Shotgun")
    if os.environ.get("T2R_DB_SOUND", "1") != "0":
        # Double Barrel shot (more unique and punchy): the over-under bank's four PCM thumps (the event's body, the DB's alone
        # since the Shorty got its Part I set) re-rendered from the game's own waves (tools/features/db_sound.py: .357 thump + sub + spikes).
        # T2R_DB_SOUND=0 = the game's original thumps; T2R_DB_SOUND_CAND picks another candidate (magnum-boom / rifle-slam / twin-cannon)
        import db_sound
        cand = os.environ.get("T2R_DB_SOUND_CAND", db_sound.DEFAULT)
        gen = os.path.join(ROOT, "tools", "local", "db-sound")
        mapping = db_sound.build_wavs(gen, cand)
        data = open(os.path.join(ROOT, "re", "sound", db_sound.BANK + ".bnk"), "rb").read()
        data = sndbank.replace_pcm(data, os.path.join(ROOT, "re", "sound", "wav", db_sound.BANK), mapping, gen)
        os.makedirs(os.path.join(STAGE, "soundbank4"), exist_ok=True)
        open(os.path.join(STAGE, "soundbank4", db_sound.BANK + ".bnk"), "wb").write(data)
        for lst in (paths, order):
            if f"soundbank4/{db_sound.BANK}.bnk" not in lst:
                last = max(i for i, x in enumerate(lst) if x.startswith("gtex1/") or x.startswith("soundbank4/"))
                lst.insert(last + 1, f"soundbank4/{db_sound.BANK}.bnk")
        print(f"sound: {db_sound.BANK} rebuilt for the Double Barrel ({cand})")
    if dc_data_patches.MILSNIPER_ON:
        # Military Sniper shot: the bolt-scar bank (M24 thumps, tkiv bolt / round foley) with the M24 sniper rifle's booms in the blooms + distant layer
        mb = dc_data_patches.MILSNIPER_BANK
        data = open(os.path.join(ROOT, "re", "sound", mb + ".bnk"), "rb").read()
        data = sndbank.replace_waves(data, open(os.path.join(ROOT, "re", "sound", dc_data_patches.MILSNIPER_SRC_BANK + ".bnk"), "rb").read(),
                                     dc_data_patches.MILSNIPER_WAVES)
        open(os.path.join(STAGE, "soundbank4", mb + ".bnk"), "wb").write(data)
        for lst in (paths, order):
            if f"soundbank4/{mb}.bnk" not in lst:
                last = max(i for i, x in enumerate(lst) if x.startswith("gtex1/") or x.startswith("soundbank4/"))
                lst.insert(last + 1, f"soundbank4/{mb}.bnk")
        print(f"sound: {mb} rebuilt for the Military Sniper (M24 booms in {len(dc_data_patches.MILSNIPER_WAVES)} layers)")
    if dc_data_patches.ASSAULT_ON:
        stage_assault(paths, order)
    if dc_data_patches.SP_BAG_FOLEY:
        # SP parity: the bag foley banks of the TAB prop variants' mesh-audio-config (dc_data_patches.SP_BAG_FOLEY). Banks outside the
        # archives an MP match reads (fol-backpack-militia lives in world-farm.psarc) ship as unchanged copies; sp-common ones load as is.
        for bank, arc in dc_data_patches.SP_BAG_FOLEY_BANKS.items():
            if arc == "sp-common.psarc":
                continue
            data = bytearray(_game_bank(bank, arc))
            inner = bytes(data[0x40:0xa0]).split(b"\0")[0].decode()
            if inner != bank:      # (the game's fol-backpack-militia.bnk carries the name "fol-backpack-ellie": a dev copy; every other bank its own)
                assert len(bank) < 0x5f and not any(data[0x40 + len(inner):0xa0]), f"{bank}.bnk: name field"
                data[0x40:0xa0] = bank.encode().ljust(0x60, b"\0")
                print(f"sound: {bank}.bnk inner name {inner} -> {bank}")
            open(os.path.join(STAGE, "soundbank4", bank + ".bnk"), "wb").write(data)
            for lst in (paths, order):
                if f"soundbank4/{bank}.bnk" not in lst:
                    last = max(i for i, x in enumerate(lst) if x.startswith("gtex1/") or x.startswith("soundbank4/"))
                    lst.insert(last + 1, f"soundbank4/{bank}.bnk")
            print(f"sound: {bank} shipped from {arc} ({len(data)} bytes, SP bag foley)")
    _prof.lap('sound banks (sndbank rebuilds)')
    wa = os.path.join(STAGE, "bin", "dc1", "weapon-art.bin")
    dc_data_patches.patch_silencer_art(wa)
    if not os.environ.get("T2R_NO_WEDGE"):
        n, sk = dc_data_patches.apply_wedge_weapon_art(os.path.join(ROOT, "re", "gamebin", "dc1", "weapon-art.bin"),
                                                       os.path.join(wedge, "weapon-art.bin"), wa)
        print(f"wedge: weapon-art {n} holster/attach fields replayed by name" + (f", SKIPPED {sk}" if sk else ""))
        if os.environ.get("T2R_PISTOL_THIGH_SLOT1"):
            print(f"pistol holster: thigh holster moved to slot one in {dc_data_patches.swap_pistol_holster_slots(wa)} "
                  f"holster defs (Abby's p-holster = thigh draw)")
    dc_modules.set_size(mdata, "weapon-art", os.path.getsize(wa))
    wg = os.path.join(STAGE, "bin", "dc1", "weapon-gameplay.bin")
    dc_data_patches.patch_weapon_gameplay(wg)
    dc_data_patches.patch_tomahawk_melee(wa, wg)      # tomahawk holds / swings / has no throw like the machete
    _prof.lap('weapon-art / wedge / gameplay patches')
    import weapon_clone
    if os.environ.get("T2R_GIFT", "1") != "0":       # Crafter gift: the brick's gameplay / art / damage entries as `gift`, drawn with the box actor (gift.c)
        weapon_clone.clone_weapon(os.path.join(STAGE, "bin", "dc1"), dc_data_patches.GIFT_SRC, dc_data_patches.GIFT, None, None, None,
                                  None, upgrades=False)       # the brick's own actors (the box look is in the brick mesh paks: gift_box_pak.py)
        dc_data_patches.make_gift_unthrowable(os.path.join(STAGE, "bin", "dc1"))      # no throw / aim arc / projectile on the gift
    weapon_clone.add_bundles_entry(os.path.join(STAGE, "bin", "dc1", "weapon-upgrades.bin"), "rifle-mpx5",
                                   "rifle-tommy", {dc_data_patches.CAN_JOINT: 0})
    weapon_clone.clone_weapon(os.path.join(STAGE, "bin", "dc1"), "rifle-vepr", dc_data_patches.M14,
                              dc_data_patches.M14_SFX, "=rifle-m14", None, "rifle-m14-dark",
                              upgrades=False)
    dc_data_patches.patch_m14_gameplay(wg)
    # Semi-Auto Rifle animations = Tommy's M14 (No Return) overlay set, not the Vepr's (the M14 held like the bolt rifle;
    # the supplied fix is for rifle-tommy-abby-player)
    weapon_clone.copy_art_fields(wa, dc_data_patches.M14, "rifle-tommy-abby-player", (0x180, 0x188))
    # Galil (purchasable rifle-galil) held exactly like the Vepr (it was held wrong): its art def's player overlay key (+0x180 / +0x188)
    # was rifle-galil -> overlay module rifle-autorifle2; now rifle-vepr -> abby-guns/rifle-boar, the Vepr's own set.  T2R_GALIL_OWN_GRIP=1 skips.
    if not os.environ.get("T2R_GALIL_OWN_GRIP"):
        weapon_clone.copy_art_fields(wa, "rifle-galil", "rifle-vepr", (0x180, 0x188))
        print("galil: player overlay key rifle-galil -> rifle-vepr (held like the Vepr)")
    weapon_clone.clone_weapon(os.path.join(STAGE, "bin", "dc1"), "pistol-beretta", dc_data_patches.ENFORCER,
                              dc_data_patches.ENFORCER_SFX, dc_data_patches.ENFORCER_AUDIO, None,
                              dc_data_patches.DEAGLE_ACTOR if DEAGLE else "pistol-cz75-dina",
                              muzzle_dz=dc_data_patches.DEAGLE_MUZZLE_DZ if DEAGLE else None, upgrades=False)
    dc_data_patches.patch_m14_gameplay(wg, dc_data_patches.ENFORCER, dc_data_patches.ENFORCER_STATS)
    weapon_clone.clone_weapon(os.path.join(STAGE, "bin", "dc1"), "shotgun-remington-pump", dc_data_patches.TAC,
                              dc_data_patches.TAC_SFX, None, None, "shotgun-pump-stock-black", upgrades=False)
    dc_data_patches.patch_m14_gameplay(wg, dc_data_patches.TAC, dc_data_patches.TAC_STATS, dc_data_patches.TAC_FALLOFF,
                                     dc_data_patches.TAC_SPREAD)
    dc_data_patches.patch_tac_reticle(wg)
    weapon_clone.clone_weapon(os.path.join(STAGE, "bin", "dc1"), dc_data_patches.TAC, dc_data_patches.TAC + "-supp",
                              dc_data_patches.TAC_SUPP_SFX, None, None, dc_data_patches.TAC_SUPP_ACTOR, None,
                              dc_data_patches.TAC_MUZZLE_SHIFT, upgrades=False)
    # 2013 Burst Rifle: a clone of the Vepr (its anims / shot / ammo / default joints = no scope, no long mag) on the kitbash
    # model rifle-brst (tools/models/burst_rifle.py), three-round bursts through the firearm def's burst-fire-settings
    weapon_clone.clone_weapon(os.path.join(STAGE, "bin", "dc1"), "rifle-vepr", dc_data_patches.BURST, dc_data_patches.BURST_SFX, dc_data_patches.BURST_AUDIO, None,
                              dc_data_patches.BURST_ACTOR, muzzle_dz=dc_data_patches.BURST_MUZZLE_DZ)
    dc_data_patches.patch_m14_gameplay(wg, dc_data_patches.BURST, dc_data_patches.BURST_STATS)
    dc_data_patches.patch_burst_fire(wg)
    if dc_data_patches.ASSAULT_ON:
        # 2013 Assault Rifle = the game's own No Return rifle (Marlene's, rifle-assault-firefly-ellie-player): NO clone - its firearm def gets
        # net10's assault-rifle-automatic numbers in place (own copy); model / sounds / hold / HUD icon / ammo are the game's (see the
        # ASSAULT block in dc_data_patches; stage_assault ships the actor pak + the M4 bank), no upgrade rows, no bundles entry
        dc_data_patches.patch_m14_gameplay(wg, dc_data_patches.ASSAULT, dc_data_patches.ASSAULT_STATS)
        dc_data_patches.verify_assault(os.path.join(STAGE, "bin", "dc1"))
    if dc_data_patches.EXPBOW_ON:
        # 2013 Launcher = the EXPLOSIVE BOW (GEAR UP purchasable row 9): a clone of Ellie's bow bound to the game's own explosive arrow (ammo-arrow-explosive:
        # model, trail, SP explosion, sounds); see the EXPBOW block in dc_data_patches. upgrades=True: the bow's visual bundles / joint list come with the
        # clone (no upgrade rows in the ext: a purchasable). The arrow's speed + the observers' burst projectile are in make_bullet_overlay.
        weapon_clone.clone_weapon(os.path.join(STAGE, "bin", "dc1"), dc_data_patches.EXPBOW_SRC, dc_data_patches.EXPBOW)
        dc_data_patches.patch_expbow_gameplay(wg)
        dc_data_patches.verify_expbow(os.path.join(STAGE, "bin", "dc1"), os.path.join(STAGE, "bin", "dc1", "bullet-settings.bin"))
    if dc_data_patches.FLAME_ON:
        # 2013 Flamethrower = TLOU2's own `flamethrower` (Abby's), no clone and no data patch: only the build check that the game's defs are what the
        # ext (flame2013.c) assumes; its actor + sound bank are in sp-common (mounted in MP), the level lists them (MP_LEVEL_ADDS)
        dc_data_patches.verify_flame(os.path.join(STAGE, "bin", "dc1"))
    if dc_data_patches.MILSNIPER_ON:
        # 2013 Military Sniper: a clone of the hunting rifle (its bolt-action clips / overlays / scope / sounds) on the Mazur LDR
        # model (tools/models/mazur.py).  upgrades=True: the scope bundle (t2r-ext gives it at spawn) and the default joint list come
        # with the clone, own copy of the bundle entry like the -supp clones'.  2013 purchasables have no upgrade rows (no UPGRADES row in the ext).
        weapon_clone.clone_weapon(os.path.join(STAGE, "bin", "dc1"), dc_data_patches.MILSNIPER_SRC, dc_data_patches.MILSNIPER, None, None, None,
                                  dc_data_patches.MILSNIPER_ACTOR, {dc_data_patches.CAN_JOINT: 0}, dc_data_patches.MILSNIPER_MUZZLE_DZ or None)
        dc_data_patches.patch_m14_gameplay(wg, dc_data_patches.MILSNIPER, dc_data_patches.MILSNIPER_STATS)
        weapon_clone.set_art_sfx(wa, dc_data_patches.MILSNIPER, dc_data_patches.MILSNIPER_SFX, dc_data_patches.MILSNIPER_AUDIO)
        dc_data_patches.patch_milsniper_scope_reticle(wg)      # own scope view (mil-dot widget), the hunting rifle keeps the shared one
        if dc_data_patches.MILSNIPER_NO_RECHAMBER:       # semi-auto at 2013's 0.4 s: no bolt cycle after the shot
            dc_data_patches.patch_milsniper_art(wa)
    _prof.lap('weapon clones (M14/Enforcer/Tac/Burst/Military Sniper)')
    if dc_data_patches.FRONTIER_ON:
        stage_frontier(paths, order, wa, wg, mdata)
    _prof.lap('stage_frontier (carriers, overlay, sound)')
    # 2013 El Diablo (purchasable scoped revolver) and Shorty (sawn-off double barrel sidearm):
    # clones of the revolver / Abby's over-under on their own models (tools/models/factions_guns.py)
    if dc_data_patches.FACTION_GUNS:
        weapon_clone.clone_weapon(os.path.join(STAGE, "bin", "dc1"), "pistol-revolver-taurus", dc_data_patches.DIABLO,
                                  None, None, None, dc_data_patches.DIABLO_ACTOR, upgrades=False)
        dc_data_patches.patch_m14_gameplay(wg, dc_data_patches.DIABLO, dc_data_patches.DIABLO_STATS)
        dc_data_patches.patch_diablo_scope(wg)          # a real scope view (the hunting pistol's scoped fields)
        if os.environ.get("T2R_NO_DIABLO_AMMO") != "1": dc_data_patches.patch_diablo_ammo(wg)           # its own ammo pool (2013 ammo-pistol-uber, max 6), not the revolver's
        weapon_clone.add_scope_entry(wu, dc_data_patches.DIABLO, "pistol-revolver-taurus", "pistol-xcaliber")
        weapon_clone.clone_weapon(os.path.join(STAGE, "bin", "dc1"), "shotgun-baserri-over-under", dc_data_patches.SHORTY,
                                  None, None, None, dc_data_patches.SHORTY_ACTOR, upgrades=False)
        dc_data_patches.patch_m14_gameplay(wg, dc_data_patches.SHORTY, dc_data_patches.SHORTY_STATS)
        if dc_data_patches.P1_GUNS:
            stage_p1_anims(paths, order, wa, mdata, wg)
    _prof.lap('El Diablo/Shorty clones + stage_p1_anims (Part I gun anims, sounds)')
    # the M14 has no upgrade sets (stat upgrades in t2r-ext); a bundles entry only for its joint
    # visibility (the suppressor can, CAN_GRAFTS)
    weapon_clone.add_bundles_entry(os.path.join(STAGE, "bin", "dc1", "weapon-upgrades.bin"), dc_data_patches.M14,
                                   "rifle-tommy", {dc_data_patches.CAN_JOINT: 0})
    weapon_clone.add_bundles_entry(os.path.join(STAGE, "bin", "dc1", "weapon-upgrades.bin"), dc_data_patches.ENFORCER,
                                   "rifle-tommy", {dc_data_patches.CAN_JOINT: 0})
    # 9mm = the Colt 1911's shot (its suppressed clone below inherits it); after the Enforcer clone, which took the Beretta's names
    weapon_clone.set_art_sfx(wa, "pistol-beretta", dc_data_patches.COLT_SFX, dc_data_patches.COLT_AUDIO)
    for base, sfx, audio, mode in dc_data_patches.SUPP_WEAPONS:
        can = base in dc_data_patches.CAN_GRAFTS or base == dc_data_patches.BURST
        joints = {dc_data_patches.CAN_JOINT: 1}
        if base == "pistol-glock":
            joints["barrel_mod"] = 0                   # compensator off: the can sits on the slide
        if base == "rifle-mpx5":
            weapon_clone.clone_weapon(os.path.join(STAGE, "bin", "dc1"), base, base + "-supp", sfx, audio, mode, None,
                                      {dc_data_patches.CAN_JOINT: 1}, dc_data_patches.FAR_MUZZLE_SHIFT)
            continue
        if base == dc_data_patches.FRONTIER:
            # Suppressed Frontier Rifle: the Frontier's clone (stats, overlay, art fields) on its own Marlin pak with the can (grafted below); no
            # upgrade rows / bundles to copy (the Frontier has none), the muzzle effects move to the can's end
            weapon_clone.clone_weapon(os.path.join(STAGE, "bin", "dc1"), base, base + "-supp", sfx, audio, mode,
                                      dc_data_patches.FRONTIER_SUPP_ACTOR, None, dc_data_patches.FRONTIER_CAN_GRAFT[5], upgrades=False)
            continue
        weapon_clone.clone_weapon(os.path.join(STAGE, "bin", "dc1"), base, base + "-supp", sfx, audio, mode,
                                  dc_data_patches.SUPP_MODEL_OF.get(base),
                                  joints if can else None,
                                  (dc_data_patches.BURST_CAN_GRAFT[5] if base == dc_data_patches.BURST
                                   else dc_data_patches.CAN_GRAFTS[base][6]) if can else None)
        if can and base != dc_data_patches.BURST:
            weapon_clone.set_default_joints(os.path.join(STAGE, "bin", "dc1", "weapon-upgrades.bin"), base,
                                            {dc_data_patches.CAN_JOINT: 0})
    if dc_data_patches.FAR_NPC_SHOT:
        # Full-Auto Rifle (MPX5) = the unsuppressed sfx-rifle-mpx5-npc shot; after the loop above, whose rifle-mpx5-supp clone copied the
        # original art def (it keeps the SP suppressed event - copy-on-write, set_art_sfx)
        weapon_clone.set_art_sfx(wa, "rifle-mpx5", dc_data_patches.FAR_SFX)
        dc_data_patches.verify_far_mpxs(os.path.join(STAGE, "bin", "dc1"), far, far_orig)
    dc_data_patches.verify_galil_stock(os.path.join(STAGE, "bin", "dc1"),
        open(os.path.join(STAGE, "soundbank4", dc_data_patches.RIFLE_SUPP_BANK + ".bnk"), "rb").read(), galil_orig)
    _prof.lap('supp weapon clones')
    # Tactical Shotgun: the circle reticle. art def +0x2c = weapon class (3 shotgun,
    # 2 rifle, 1 pistol) - try rifle for the reticle (+0x1c0 was not it)
    # (+0x2c weapon class 2 gave it the bolt rifle's rechamber animation - reverted; reticle open)
    # Double Barrel: 2013 big circle (after the Shorty clone, which copies the over-under and keeps its own reticle)
    dc_data_patches.patch_tac_reticle(wg, dc_data_patches.DBL_WEAPON, dc_data_patches.DBL_RETICLES)
    dc_data_patches.patch_tac_art_reticle(os.path.join(STAGE, "bin", "dc1", "weapon-art.bin"))
    if dc_data_patches.DBL_MODEL_ON:
        # Double Barrel on Abby's side-by-side model (tools/models/dbl_barrel.py; pak shipped below with the other actors)
        weapon_clone.set_art_model(os.path.join(STAGE, "bin", "dc1", "weapon-art.bin"), dc_data_patches.DBL_WEAPON,
                                   dc_data_patches.DBL_ACTOR, dc_data_patches.DBL_MUZZLE)
    weapon_clone.set_art_strings(os.path.join(STAGE, "bin", "dc1", "weapon-art.bin"),
                                 {(w, 0x70): icon for w, icon in dc_data_patches.ART_ICONS.items()})
    # art def +0x90 = the gun's notification icon (hud/notification/notifications_icons/<donor>-notification.png: the pickup / upgrade
    # popup): after an upgrade the 9mm showed the stock Beretta picture. Every gun with its own HUD icon gets that icon here too.
    weapon_clone.set_art_strings(os.path.join(STAGE, "bin", "dc1", "weapon-art.bin"),
                                 {(w, 0x90): icon for w, icon in dc_data_patches.ART_ICONS.items()}, only_if_set=True)
    # weapon-upgrade-icons.bin (base game file, not in the mod tree before): once a gun is upgraded the HUD shows these per-combination
    # icons instead of the art def's +0x70 one (the upgraded 9mm turned back into the Beretta). Every gun with its own
    # icon (and its clones) gets that icon on all upgrade combinations; shipped like the other dc1 bins (manifest + modules.bin size).
    wui = os.path.join(STAGE, "bin", "dc1", "weapon-upgrade-icons.bin")
    ulog = weapon_clone.patch_upgrade_icons(os.path.join(ROOT, "re", "gamebin", "dc1", "weapon-upgrade-icons.bin"), wui,
                                            dc_data_patches.ART_ICONS, weapon_clone.CLONES)
    assert ulog, "weapon-upgrade-icons: no weapon patched"
    for w, src, ent in ulog:
        print(f"upgrade icons: {w} (entries of {src}) " + ", ".join(f"{k:#x}:{old.rsplit('/', 1)[-1]}->{new.rsplit('/', 1)[-1]}" for k, old, new in ent))
    print(f"upgrade icons: {len(ulog)} weapons patched, {sum(len(e) for _, _, e in ulog)} entries")
    if "bin/dc1/weapon-upgrade-icons.bin" not in paths:
        for lst in (paths, order):
            last = max(i for i, x in enumerate(lst) if x.startswith("bin/dc1/"))
            lst.insert(last + 1, "bin/dc1/weapon-upgrade-icons.bin")
    dc_modules.set_size(mdata, "weapon-upgrade-icons", os.path.getsize(wui))
    if os.environ.get("T2R_PISTOL_THIGH_SLOT1") and not os.environ.get("T2R_NO_WEDGE"):
        # after every clone: the revolver family, El Diablo and Shorty have no contributed thigh slot (back holster otherwise)
        print("sidearm holster: " + str(dc_data_patches.force_sidearm_thigh_slot(os.path.join(STAGE, "bin", "dc1", "weapon-art.bin"))))
        print("second sidearm slot: %d holster defs" % dc_data_patches.make_second_sidearm_slot(os.path.join(STAGE, "bin", "dc1", "weapon-art.bin")))
    if os.environ.get("T2R_HOLSTER_FIT", "1") != "0":
        # per-gun holster fit (tools/models/holster_fit.py -> data/holster_fit.json; re/holster-fit.md): every sidearm's own holster offset
        print("sidearm holster fit: " + str(dc_data_patches.fit_sidearm_holsters(os.path.join(STAGE, "bin", "dc1", "weapon-art.bin"))))
    if dc_data_patches.SCAR_HOLSTER_ON:
        # per-gun Seraphite back-holster points: those sidearms' slot one asks t2r_ph_<art> (= the thigh point elsewhere; t2r-ext pist_map)
        print("scar holster guns: " + str(dc_data_patches.alias_sidearm_slot_one(os.path.join(STAGE, "bin", "dc1", "weapon-art.bin"))))
    if dc_data_patches.SECOND_HOLSTER_ON:
        # second sidearm (slot two, t2r_pistol_second): every sidearm's barrel / slide where the Glock's sits in the second holsters
        print("second sidearm slot two: " + str(dc_data_patches.second_slot_offsets(os.path.join(STAGE, "bin", "dc1", "weapon-art.bin"))))
    # backpack weapon fit: every stowable weapon gets its own holster attach objects (unique offset key for the ext's bp_wpn_fit.h)
    sys.path.insert(0, os.path.join(TOOLS, "models"))
    import backpack_wpn_fit
    # ON by default (T2R_BP_UNIQUE=0 opts out). The crash at exe+1cd488c was our attach census calling exe fn
    # 0x1CD4870 on stale weapon processes (78ec5c1), not these copies: copy_obj verified offline on the staged weapon-art.bin
    # (53 attach objects: every pointer slot relocated, bitmap consistent, all copies equal their source except pos.x low bits).
    if os.environ.get("T2R_BP_UNIQUE", "1") != "0":
        print("backpack weapon fit: %d holster attach objects made unique" % dc_data_patches.unique_holster_offsets(
            os.path.join(STAGE, "bin", "dc1", "weapon-art.bin"), backpack_wpn_fit.art_names()))
    if dc_data_patches.SCAR_HOLSTER_ON or os.environ.get("T2R_HOLSTER_FIT", "1") != "0":
        # revolver / El Diablo carry Abby's own holster data at art def +0x158 (the Seraphite bodies read it): same slot objects as +0x150's
        print("abby holster data (revolver family): " + str(dc_data_patches.abby_holster_data_as_ellie(os.path.join(STAGE, "bin", "dc1", "weapon-art.bin"))))
    if os.environ.get("T2R_DB_STOW_SP", "1") != "0":
        # the stowed Double Barrel and Vepr exactly where SP puts Abby's (their own slot offsets only, backpack_slot2.py section 4)
        print("double barrel + vepr stow (SP place): " + str(dc_data_patches.adjust_db_stow(os.path.join(STAGE, "bin", "dc1", "weapon-art.bin"))))
    _prof.lap('double barrel reticle / art patches')
    sys.path.insert(0, os.path.join(TOOLS, "models"))
    deagle_pak = None
    if DEAGLE:
        # the Enforcer's Desert Eagle (still under the CZ75's package names: the can graft below
        # works on it, then it is renamed to DEAGLE_ACTOR)
        import deagle
        deagle_pak = os.path.join(STAGE, "actor97", "deagle-work.pak")
        deagle.build(deagle_pak, textures=DEAGLE_TEX)
        print(f"model: Enforcer = Desert Eagle ({'own' if DEAGLE_TEX else 'CZ75'} textures)")
    # the pistol gun clips drive extra slide joints (re/enforcer-slide.md, re/colt-slide.md): slide_base (the Deagle's, K =
    # T2R_DEAGLE_SLIDE) and slide_mod (the Part I Colt's, COLT_K = T2R_COLT_SLIDE); 1 = off
    import deagle_slide
    slide_joints = deagle_slide.joint_ks(DEAGLE, dc_data_patches.P1_COLT)
    if slide_joints:
        for pak, rep in deagle_slide.build(os.path.join(STAGE, "actor97"), deagle=DEAGLE, colt=dc_data_patches.P1_COLT):
            deagle_slide.sanity(pak, open(os.path.join(STAGE, "actor97", pak), "rb").read(), rep)
            p = "actor97/" + pak
            for lst in (paths, order):
                if p not in lst:
                    anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
                    lst.insert(lst.index(anchor) + 1, p)
            print(f"anim: {pak} - slide joints {slide_joints} in {len(rep)} clips "
                  f"({sum(1 for r in rep if r[1] == 'const')} constant, {sum(1 for r in rep if r[1] != 'const')} re-encoded)")
    _prof.lap('model: Deagle build + slide anims')
    # El Diablo (revolver + xcaliber scope, pak editor graft) and Shorty (sawn-off over-under), own
    # actors listed in actor97.txt / the MP level (dc_data_patches.MP_LEVEL_ADDS)
    import factions_guns
    fg_paks = {}
    for actor, make in (((dc_data_patches.DIABLO_ACTOR, lambda o: factions_guns.build_diablo(o, *factions_guns.pak_cli())),
                         (dc_data_patches.SHORTY_ACTOR, factions_guns.build_shorty)) if dc_data_patches.FACTION_GUNS else ()):
        fgo = os.path.join(STAGE, "actor97", actor + ".pak")
        fg_paks[actor] = _cached_pak("model-" + actor, ["factions_guns", "p1_guns"], fgo, lambda: make(fgo),
                                     (_p1_inputs() if factions_guns.p1_guns_enabled() else [_models()]))
        p = "actor97/" + actor + ".pak"
        for lst in (paths, order):
            if p not in lst:
                anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
                lst.insert(lst.index(anchor) + 1, p)
    # No Return supply crate (tools/ext/nrbox.c): the SP actor paks ship in the mod so the MP level always finds them (world-rogue.psarc is an SP world archive;
    # the level's pak list names them, T2R_NRBOX=0 builds without). ~2.1 MB.
    if os.environ.get("T2R_NRBOX", "1") != "0":
        import dsar_list
        _wr = os.path.join(os.environ.get("T2_GAME", "C:/Games/The Last of Us Part II Remastered"), "build", "pc", "main", "world-rogue.psarc")
        _d, _ent, _bs, _bsz = dsar_list.open_files(_wr)
        for _a in ("rl-cache-crate-military", "rl-cache-crate-skel"):
            _p = "actor97/" + _a + ".pak"
            os.makedirs(os.path.join(STAGE, "actor97"), exist_ok=True)
            open(os.path.join(STAGE, _p), "wb").write(dsar_list.extract(_d, _ent, _bs, _bsz, _p))
            for lst in (paths, order):
                if _p not in lst:
                    anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
                    lst.insert(lst.index(anchor) + 1, _p)
    # Crafter gift (tools/ext/gift.c): the gift weapon IS the brick (clone_weapon brick -> gift, its actors); its four mesh paks throw-brick-standard / -a / -b / -c
    # carry the SP medical supply box instead (tools/models/gift_box_pak.py: box mesh in every LOD, box maps in the brick material) - joints, grip, hold and
    # throw are the brick's own. The MP level did not load the brick actors: dc_data_patches.MP_LEVEL_ADDS adds them. T2R_GIFT=0 builds without.
    if os.environ.get("T2R_GIFT", "1") != "0":
        import gift_box_pak
        for _out in gift_box_pak.build(STAGE):
            _p = "actor97/" + os.path.basename(_out)
            for lst in (paths, order):
                if _p not in lst:
                    anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
                    lst.insert(lst.index(anchor) + 1, _p)
    _prof.lap('model: factions_guns (El Diablo + Shorty)')
    # Burst Rifle model (actor rifle-brst, a Vepr kitbash; its own pak in the MP level, dc_data_patches.MP_LEVEL_ADDS)
    import burst_rifle
    assert burst_rifle.BURST_ACTOR == dc_data_patches.BURST_ACTOR, "burst_rifle actor != dc_data_patches.BURST_ACTOR"
    assert round(burst_rifle.BARREL_SHORTEN, 4) == round(dc_data_patches.BURST_MUZZLE_DZ, 4), "burst_rifle barrel != BURST_MUZZLE_DZ"
    burst_pak = burst_rifle.build(os.path.join(STAGE, "actor97", dc_data_patches.BURST_ACTOR + ".pak"))
    burst_rifle.verify(burst_pak)
    p = "actor97/" + dc_data_patches.BURST_ACTOR + ".pak"
    for lst in (paths, order):
        if p not in lst:
            anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
            lst.insert(lst.index(anchor) + 1, p)
    _prof.lap('model: burst_rifle')
    # Military Sniper model (actor rifle-milsniper-mazr: the hunting rifle's pak with the UC4 Mazur LDR, tools/models/mazur.py)
    mil_pak = None
    if dc_data_patches.MILSNIPER_ON:
        import mazur
        assert mazur.ACTOR == dc_data_patches.MILSNIPER_ACTOR, "mazur.ACTOR != dc_data_patches.MILSNIPER_ACTOR"
        assert mazur.HOST_ACTOR == dc_data_patches.MILSNIPER_SRC, "mazur host actor != MILSNIPER_SRC"
        mil_pak = os.path.join(STAGE, "actor97", dc_data_patches.MILSNIPER_ACTOR + ".pak")
        _cached_pak("model-milsniper", ["mazur", "mazur_tex", "ps4tex", "mesh_replace", "rename_pak"], mil_pak,
                    lambda: mazur.build_actor(mil_pak),
                    [_models(), _bc.Stat(os.path.join(ROOT, "re", "milsniper")),
                     _bc.File(os.path.join(ROOT, "tools", "features", "data", "milsniper-scope-reticle.png"))])
        mazur.verify(mil_pak)
        p = "actor97/" + dc_data_patches.MILSNIPER_ACTOR + ".pak"
        for lst in (paths, order):
            if p not in lst:
                anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
                lst.insert(lst.index(anchor) + 1, p)
    _prof.lap('model: mazur (Military Sniper)')
    # Double Barrel: Abby's side-by-side double barrel, shells on the over-under's joints (tools/models/dbl_barrel.py).
    # Shipped under the game's own actor name (the game's copy lives in world-farm.psarc, not in the MP level's archives)
    dbl_pak = None
    if dc_data_patches.DBL_MODEL_ON:
        import dbl_barrel
        assert dbl_barrel.ACTOR == dc_data_patches.DBL_ACTOR, "dbl_barrel.ACTOR != DBL_ACTOR"
        dbl_pak = dbl_barrel.build(os.path.join(STAGE, "actor97", dbl_barrel.ACTOR + ".pak"))
        dbl_barrel.verify(dbl_pak)
        p = "actor97/" + dbl_barrel.ACTOR + ".pak"
        for lst in (paths, order):
            if p not in lst:
                anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
                lst.insert(lst.index(anchor) + 1, p)
    _prof.lap('model: dbl_barrel')
    # visible armour: grey copies of the cut multiplayer's helmet / torso (tools/models/armour_paks.py)
    import armour_paks
    adir = os.path.join(STAGE, "actor97")
    arm = {a: os.path.join(adir, a + ".pak") for a in (armour_paks.HELMET_ACTOR, armour_paks.TORSO_ACTOR)}

    def make_arm():
        got = armour_paks.build(adir)
        assert got == arm, (got, arm)
    _bc.cached("armour-paks", [_bc.Code("armour_paks"), _env_scoped("armour_paks"), _models(), _re("models", "armor-ref"), _re("armor"), _bc.File(armour_paks.pak_cli()[0])], make_arm, files=list(arm.values()))
    _verify_cached("armour_paks", ["armour_paks"], list(arm.values()), lambda: armour_paks.verify(arm), extra=[_models(), _re("models", "armor-ref")])
    for actor in arm:
        p = "actor97/" + actor + ".pak"
        for lst in (paths, order):
            if p not in lst:
                anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
                lst.insert(lst.index(anchor) + 1, p)
    print(f"model: grey armour {', '.join(arm)}")
    _prof.lap('model: armour_paks (grey armour)')
    # the supply satchel's visible bag (tools/models/satchel_pak.py, re/satchel-1102.md): an NPC backpack mesh as its own actor
    import satchel_pak
    sat = {satchel_pak.ACTOR: os.path.join(adir, satchel_pak.ACTOR + ".pak")}
    assert satchel_pak.ACTOR == dc_data_patches.SATCHEL_ACTOR

    def make_sat():
        satchel_pak.build(adir)
    _bc.cached("satchel-pak", [_bc.Code("satchel_pak"), _bc.Code("armour_paks"), _env_scoped("satchel_pak", "armour_paks"), _models()], make_sat, files=list(sat.values()))
    for actor in sat:
        p = "actor97/" + actor + ".pak"
        for lst in (paths, order):
            if p not in lst:
                anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
                lst.insert(lst.index(anchor) + 1, p)
    print(f"model: supply satchel {', '.join(sat)}")
    _prof.lap('model: satchel_pak')
    # Joel locomotion paks (tools/models/joel_mm.py, re/joelanims/ANALYSIS.md): the five contributed Part I Joel paks renamed to anim-joel-player-mm-* (in place, ~44 MB); T2R_JOEL_MM=0 builds without
    # tools/ext/joelmm_built.h (generated, git-ignored): the ext's joelmm module runs only when THIS build carries the joel paks. Without them its
    # pose-DB sid rewrite points the motion matcher at sets that do not exist (sprint kept walking after the keys were released).
    with open(os.path.join(TOOLS, "ext", "joelmm_built.h"), "w", newline="\n") as f:
        f.write("// generated by build_dll.py (T2R_JOEL_MM)\n#define JOELMM_BUILT %d\n" % (1 if dc_data_patches.JOEL_MM_ON else 0))
    if dc_data_patches.JOEL_MM_ON:
        import joel_mm
        assert joel_mm.JOEL_PAKS == dc_data_patches.JOEL_MM_ACTORS, 'joel_mm.JOEL_PAKS != dc_data_patches.JOEL_MM_ACTORS'
        joel = {a: os.path.join(adir, a + '.pak') for a in joel_mm.JOEL_PAKS}

        def make_joel():
            joel_mm.build(adir)
        _bc.cached('joel-mm-paks', [_bc.Code('joel_mm'), _env_scoped('joel_mm')] + [_bc.File(os.path.join(joel_mm.src_dir(), n + '.psarc')) for n in joel_mm.PAKS], make_joel, files=list(joel.values()))
        for actor in joel:
            p = 'actor97/' + actor + '.pak'
            for lst in (paths, order):
                if p not in lst:
                    anchor = 'actor97/gui2-widgets-pc.pak' if 'actor97/gui2-widgets-pc.pak' in lst else 'actor97/gui2-widgets.pak'
                    lst.insert(lst.index(anchor) + 1, p)
        print(f"model: Joel locomotion {', '.join(joel)}")
        _prof.lap('model: joel_mm')

    # held bag props of the puppets (tools/models/held_bag_pak.py): each look's own bag part at full scale as its own actor (fallback when the TAB prop look cannot be spawned animated)
    import held_bag_pak
    assert sorted(held_bag_pak.ACTORS.values()) == sorted(dc_data_patches.HELD_BAG_ACTORS)
    hb = {a: os.path.join(adir, a + ".pak") for a in held_bag_pak.ACTORS.values()}

    def make_hb():
        held_bag_pak.build(adir)
    _bc.cached("held-bag-pak", [_bc.Code("held_bag_pak"), _bc.Code("armour_paks"), _env_scoped("held_bag_pak", "armour_paks"), _models()], make_hb, files=list(hb.values()))
    for actor in hb:
        p = "actor97/" + actor + ".pak"
        for lst in (paths, order):
            if p not in lst:
                anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
                lst.insert(lst.index(anchor) + 1, p)
    print(f"model: held bags {', '.join(hb)}")
    _prof.lap('model: held_bag_pak')
    # Rattler armour parts for the armoured looks (tools/models/armour_rattler.py, re/armor-rattler.md): the
    # helmet is a game pak (level add only), the vest a renamed slaver-male-04 kit copy with only the vest drawing
    import armour_rattler
    rat_rep = {}
    rat = armour_rattler.build(os.path.join(STAGE, "actor97"), report=rat_rep)
    armour_rattler.verify(rat)
    assert (dc_data_patches.ARMOUR_RIG_HELMET, dc_data_patches.ARMOUR_RIG_TORSO, dc_data_patches.ARMOUR_RIG_VEST_ACTOR) == \
        (armour_rattler.HELMET_ACTOR, armour_rattler.VEST_PART, armour_rattler.VEST_ACTOR), "armour_rattler names != dc_data_patches"
    p = "actor97/" + armour_rattler.VEST_ACTOR + ".pak"
    for lst in (paths, order):
        if p not in lst:
            anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
            lst.insert(lst.index(anchor) + 1, p)
    print(f"model: Rattler armour {armour_rattler.HELMET_ACTOR} (game pak) + {armour_rattler.VEST_ACTOR} {rat_rep}")
    _prof.lap('model: armour_rattler')
    # clothes pushed in under the armour / backpacks pulled over it: the copies the twins swap in
    # (tools/models/armour_push.py; the rigged backpacks are built first - the later backpack block reuses the cache)
    if dc_data_patches.ARMOUR_UNDER_ON:
        import armour_push
        assert armour_push.SOURCES == dc_data_patches.ARMOUR_UNDER_SOURCES, "armour_push.SOURCES != ARMOUR_UNDER_SOURCES"
        assert armour_push.PROPS == dc_data_patches.ARMOUR_UNDER_PROPS, "armour_push.PROPS != ARMOUR_UNDER_PROPS"
        bags = {}
        if dc_data_patches.BACKPACK_RIG_ON:
            import backpack_rig
            bags = backpack_rig.build(os.path.join(STAGE, "actor97"))
        un_rep = {}
        under = armour_push.build(os.path.join(STAGE, "actor97"), rat, bags, report=un_rep)
        _verify_cached("armour_push", ["armour_push", "armour_warp"],
                       sorted(set(list(under.values()) + list(rat.values()) + list(bags.values()) +
                                  [armour_push.source_pak(s, bags) for s in armour_push.SOURCES])),
                       lambda: armour_push.verify(under, rat, bags), stats=[os.path.join(ROOT, "re", "armor"), os.path.join(ROOT, "re", "models", "armor-ref")])
        assert sorted(under) == sorted(dc_data_patches.ARMOUR_UNDER_COPIES), (sorted(under), sorted(dc_data_patches.ARMOUR_UNDER_COPIES))
        for actor in under:
            p = "actor97/" + actor + ".pak"
            for lst in (paths, order):
                if p not in lst:
                    anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
                    lst.insert(lst.index(anchor) + 1, p)
        print(f"model: armour push / pull body copies {len(under)} {un_rep}")
    _prof.lap('model: armour_push (+ backpack_rig first build)')
    # female armoured twins (tools/models/armour_female.py, re/armor-rattler.md "Female"): the Rattler helmet + plate carrier on
    # base-female-skel (renamed kit copies, only the armour draws) and the female looks' pushed clothes copies
    if dc_data_patches.ARMOUR_RIG_FEMALE_ON:
        import armour_female
        assert (dc_data_patches.ARMOUR_RIG_F_HELMET, dc_data_patches.ARMOUR_RIG_F_TORSO, dc_data_patches.ARMOUR_RIG_F_HELMET_ACTOR,
                dc_data_patches.ARMOUR_RIG_F_VEST_ACTOR) == (armour_female.HELMET_PART, armour_female.VEST_PART, armour_female.HELMET_ACTOR,
                                                              armour_female.VEST_ACTOR), "armour_female names != dc_data_patches"
        if dc_data_patches.ARMOUR_UNDER_ON:
            assert armour_female.SOURCES == dc_data_patches.ARMOUR_UNDER_SOURCES_F or not dc_data_patches.ARMOUR_UNDER_PROPS_F, \
                "armour_female.SOURCES != ARMOUR_UNDER_SOURCES_F"
        f_rep = {}
        frat = armour_female.build(os.path.join(STAGE, "actor97"), report=f_rep)
        _verify_cached("armour_female", ["armour_female"], list(frat.values()), lambda: armour_female.verify(frat),
                       stats=[os.path.join(ROOT, "re", "models", "armor-ref")])
        fcopies = {}
        if dc_data_patches.ARMOUR_UNDER_ON:
            fcopies = armour_female.build_copies(os.path.join(STAGE, "actor97"), frat, report=f_rep)
            _verify_cached("armour_female_copies", ["armour_female", "armour_push", "armour_warp"], sorted(set(list(fcopies.values()) + list(frat.values()))),
                           lambda: armour_female.verify_copies(fcopies, frat), stats=[os.path.join(ROOT, "re", "models", "armor-ref")])
            assert sorted(fcopies) == sorted(dc_data_patches.ARMOUR_UNDER_COPIES_F), (sorted(fcopies), sorted(dc_data_patches.ARMOUR_UNDER_COPIES_F))
        for actor in list(frat) + list(fcopies):
            p = "actor97/" + actor + ".pak"
            for lst in (paths, order):
                if p not in lst:
                    anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
                    lst.insert(lst.index(anchor) + 1, p)
        print(f"model: female Rattler armour {', '.join(frat)} + {len(fcopies)} pushed copies {f_rep}")
    _prof.lap('model: armour_female')
    # player-style rigged look backpacks (tools/models/backpack_rig.py, re/backpack-rig.md)
    if dc_data_patches.BACKPACK_RIG_ON:
        import backpack_rig
        assert backpack_rig.RIGGED == dc_data_patches.BACKPACK_RIG, "backpack_rig.RIGGED != dc_data_patches.BACKPACK_RIG"
        bp_rep = {}
        bps = backpack_rig.build(os.path.join(STAGE, "actor97"), report=bp_rep)
        backpack_rig.verify(bps)
        for actor in bps:
            p = "actor97/" + actor + ".pak"
            for lst in (paths, order):
                if p not in lst:
                    anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
                    lst.insert(lst.index(anchor) + 1, p)
        print(f"model: rigged backpacks {', '.join(bps)} {bp_rep}")
    _prof.lap('model: backpack_rig')
    # TAB-menu backpack props (tools/models/backpack_prop.py): the look bags on the backpack-abby prop skeleton
    if dc_data_patches.BACKPACK_PROPS_ON:
        import backpack_prop
        assert backpack_prop.PROPS == dc_data_patches.BACKPACK_PROPS, "backpack_prop.PROPS != dc_data_patches.BACKPACK_PROPS"
        pp_rep = {}
        pps = backpack_prop.build(os.path.join(STAGE, "actor97"), report=pp_rep)
        backpack_prop.verify(pps)
        for actor in pps:
            p = "actor97/" + actor + ".pak"
            for lst in (paths, order):
                if p not in lst:
                    anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
                    lst.insert(lst.index(anchor) + 1, p)
        print(f"model: TAB backpack props {', '.join(pps)} {pp_rep}")
    _prof.lap('model: backpack_prop')
    # Seraphite back-of-belt pistol holster (tools/models/scar_back_holster.py): Abby's holster_back on base-male / base-female pelvis
    if dc_data_patches.SCAR_HOLSTER_ON:
        import scar_back_holster
        assert scar_back_holster.OUT == {g[0]: a for g, a in dc_data_patches.SCAR_HOLSTER_PARTS.items()}, \
            "scar_back_holster.OUT != dc_data_patches.SCAR_HOLSTER_PARTS"
        shs = scar_back_holster.build(os.path.join(STAGE, "actor97"))
        scar_back_holster.verify(shs)
        for actor in shs:
            p = "actor97/" + actor + ".pak"
            for lst in (paths, order):
                if p not in lst:
                    anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
                    lst.insert(lst.index(anchor) + 1, p)
        print(f"model: Seraphite back holster {', '.join(shs)}")
    _prof.lap('model: scar_back_holster')
    # second-sidearm holsters (tools/models/second_holster.py): WLF back holsters per look group, Seraphite chest rigs per jacket cut
    if dc_data_patches.SECOND_HOLSTER_ON:
        import second_holster
        assert sorted(second_holster.actors()) == sorted(set(dc_data_patches.second_holster_parts().values())), \
            "second_holster.actors() != dc_data_patches.second_holster_parts()"
        s2 = second_holster.build(os.path.join(STAGE, "actor97"))
        second_holster.verify(s2)
        for actor in s2:
            p = "actor97/" + actor + ".pak"
            for lst in (paths, order):
                if p not in lst:
                    anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
                    lst.insert(lst.index(anchor) + 1, p)
        print(f"model: second-sidearm holsters {', '.join(s2)}")
    _prof.lap('model: second_holster')
    import make_icons
    idir = os.path.join(STAGE, "gtex1", "streaming", "t2r", "icons")
    frontier_pak = None
    if dc_data_patches.FRONTIER_ON:        # the Marlin lever model (game pak) for the Frontier Rifle's icon
        frontier_pak = os.path.join(STAGE, "actor97", "frontier-icon-src.pak")
        open(frontier_pak, "wb").write(_game_pak(dc_data_patches.FRONTIER_ACTOR))
    icon_paks = dict(enforcer_pak=deagle_pak, colt_pak=_colt_work_pak() if dc_data_patches.P1_COLT else None,
                     diablo_pak=fg_paks.get(dc_data_patches.DIABLO_ACTOR), shorty_pak=fg_paks.get(dc_data_patches.SHORTY_ACTOR),
                     dbl_pak=dbl_pak, frontier_pak=frontier_pak, burst_pak=burst_pak, milsniper_pak=mil_pak)
    _bc.cached("icons", [_bc.Code("make_icons"), _env_scoped("make_icons"), _bc.File(os.path.join(TOOLS, "features", "data", "milsniper-scope-reticle.png")), _re("icons"), _models(), _re("armor", "ref"), _bc.Stat(os.path.join(TOOLS, "models")),
                         {k: (_bc.File(v) if v else None) for k, v in icon_paks.items()}],
               lambda: make_icons.main(os.path.join(ROOT, "re", "icons"), idir, **icon_paks), dirs=[idir])
    if frontier_pak:
        os.remove(frontier_pak)
    for f in sorted(os.listdir(idir)):
        p = "gtex1/streaming/t2r/icons/" + f
        for lst in (paths, order):
            if p not in lst:
                last = max(i for i, x in enumerate(lst) if x.startswith("gtex1/"))
                lst.insert(last + 1, p)
    print(f"icons: {len(os.listdir(idir))} files in gtex1/streaming/t2r/icons")
    _prof.lap('icons: make_icons (HUD gun icons)')
    # CHOOSE YOUR SIDE faction figures (to1/clan-side, rendered from the MP character models by
    # tools/models/figures/make_figures.py) -> streaming/t2r/clan/*.png
    sys.path.insert(0, os.path.join(TOOLS, "models", "figures"))
    import ship
    fdir = os.path.join(STAGE, "gtex1", "streaming", "t2r", "clan")
    for f in ship.main(os.path.join(ROOT, "re", "figures"), fdir):
        p = "gtex1/streaming/t2r/clan/" + f
        for lst in (paths, order):
            if p not in lst:
                last = max(i for i, x in enumerate(lst) if x.startswith("gtex1/"))
                lst.insert(last + 1, p)
    print(f"figures: {len(os.listdir(fdir))} files in gtex1/streaming/t2r/clan")
    # the clan end screens (won / lost: parallax key art, chalk rank strokes, tally, masks; tools/features/clanend_widgets.py,
    # tools/ext/clanend.c) -> streaming/t2r/clanend/*.png from re/clan-end/tex (re/win-screen-1112/src/make_ingame.py)
    import clanend_ship
    cdir = os.path.join(STAGE, "gtex1", "streaming", "t2r", "clanend")
    for f in clanend_ship.main(os.path.join(ROOT, "re", "clan-end", "tex"), cdir):
        p = "gtex1/streaming/t2r/clanend/" + f
        for lst in (paths, order):
            if p not in lst:
                last = max(i for i, x in enumerate(lst) if x.startswith("gtex1/"))
                lst.insert(last + 1, p)
    print(f"clan end screens: {len(os.listdir(cdir))} files in gtex1/streaming/t2r/clanend")
    import widgets_patch
    if dc_data_patches.MILSNIPER_ON and os.path.isfile(widgets_patch.MIL_SCOPE_PNG):     # artist's Military Sniper scope reticle (widgets_patch.add_mil_scope)
        sp = os.path.join(STAGE, "gtex1", *widgets_patch.MIL_SCOPE_TEX.split("/"))
        os.makedirs(os.path.dirname(sp), exist_ok=True)
        shutil.copyfile(widgets_patch.MIL_SCOPE_PNG, sp)
        p = "gtex1/" + widgets_patch.MIL_SCOPE_TEX
        for lst in (paths, order):
            if p not in lst:
                last = max(i for i, x in enumerate(lst) if x.startswith("gtex1/"))
                lst.insert(last + 1, p)
        print("scope: Military Sniper reticle PNG staged " + p)
    import loading_art                                  # TWELVE WEEKS entry screen (connecting_widgets variant L): title + torn bar textures
    for src, tex in ((loading_art.TITLE_PNG, loading_art.TITLE_TEX), (loading_art.BAR_PNG, loading_art.BAR_TEX), (loading_art.FEATHER_PNG, loading_art.FEATHER_TEX)):
        sp = os.path.join(STAGE, "gtex1", *tex.split("/"))
        os.makedirs(os.path.dirname(sp), exist_ok=True)
        shutil.copyfile(src, sp)
        p = "gtex1/" + tex
        for lst in (paths, order):
            if p not in lst:
                last = max(i for i, x in enumerate(lst) if x.startswith("gtex1/"))
                lst.insert(last + 1, p)
    print("entry screen: title + bar + feather textures staged (streaming/t2r/load)")
    _prof.lap('figures: clan-side faction figures (ship.main)')
    for f in ("weapon-art", "weapon-damages", "weapon-upgrades"):
        dc_modules.set_size(mdata, f, os.path.getsize(os.path.join(STAGE, "bin", "dc1", f + ".bin")))
    dc_modules.set_size(mdata, "weapon-gameplay", os.path.getsize(wg))
    open(modules, "wb").write(mdata)
    _prof.lap('modules.bin sizes')
    if sp_maps_build.selected():       # the SP-map level sets / tasks the way the game's loader sees them (relocation, sorted entry table, sizes, exports)
        import dc_check
        for k, v in dc_check.check_mod_tree(STAGE).items():
            print(f"dc_check {k}: {v[0]} entries, {v[1]} relocated pointers, {v[2]} exports - OK")
    _prof.lap('dc_check (level set loader check)')
    # Cut multiplayer armor: 4 helmet + 4 torso spawners (one per player slot, AutoSpawn off), spawned
    # on demand with spawn-entity-async (which streams the art in) and attached by t2r-ext
    subprocess.run(["pwsh", "-NoProfile", "-File", os.path.join(TOOLS, "features", "art_spawners.ps1"),
                    "-Pak", os.path.join(STAGE, "pak68", "starlight-mp-test-ingame.pak"),
                    "-Src", "to1-interact-generic-1", "-Add", ",".join(f"t2r-art-armor-{k}-{i}={a}" for i in range(4) for k, a in (("helmet", dc_data_patches.ARMOUR_HELMET_ACTOR), ("torso", dc_data_patches.ARMOUR_TORSO_ACTOR)))], check=True)
    _prof.lap('art_spawners.ps1 (pwsh)')
    dc_data_patches.patch_pipe_bomb_text(os.path.join(STAGE, "text2", "eng.common"))
    dc_data_patches.patch_recipe_texts(os.path.join(STAGE, "text2", "eng.common"))
    for _lang in ("eng.common", "uke.common"):
        if os.path.exists(os.path.join(STAGE, "text2", _lang)):
            dc_data_patches.patch_text_ciphers(os.path.join(STAGE, "text2", _lang))   # OPTIONS > MULTIPLAYER: BOW AIM ARC / CHANGE NAME labels
    for level, src in ({} if os.environ.get("T2R_NO_NAV") else dc_data_patches.NAV_LEVELS).items():
        nav = level + "-nav-ingame"
        k, n = dc_data_patches.make_nav_pak(os.path.join(ROOT, "re", "modfs", "pak68", src),
                                            os.path.join(STAGE, "pak68", nav + ".pak"))
        dc_data_patches.patch_pak68(os.path.join(STAGE, "pak68.txt"), level, [f"package {nav}"])
        for lst in (paths, order):
            if "pak68/" + nav + ".pak" not in lst:
                lst.insert(lst.index("pak68/" + src) + 1, "pak68/" + nav + ".pak")
        print(f"navmesh: {level} + package {nav} (base pak, {k} spawner names renamed, {n} strings)")
    _prof.lap('pipe bomb text + nav paks')
    lm = "bin/dc1/listen-mode.bin"
    r = dc_data_patches.patch_listen_mode(os.path.join(ROOT, "re", "gamebin", "dc1", "listen-mode.bin"),
                                          os.path.join(STAGE, *lm.split("/")))
    print("listen mode range: " + ", ".join(f"{a:.0f} -> {b:.0f} m" for a, b in r))
    for lst in (paths, order):
        if lm not in lst:
            lst.insert(lst.index("bin/dc1/weapon-gameplay.bin") + 1, lm)
    # menu sounds like the 2013 loadout menu: gui2-game.bin *menu2-default-sounds* swaps (T2R_NO_MENU_SND=1 skips)
    if not os.environ.get("T2R_NO_MENU_SND"):
        gm = "bin/dc1/gui2-game.bin"
        sw = dc_data_patches.patch_menu_sounds(os.path.join(ROOT, "re", "gamebin", "dc1", "gui2-game.bin"),
                                               os.path.join(STAGE, *gm.split("/")))
        print("menu sounds: " + ", ".join(f"{k} -> {n}" for k, _, n in sw))
        for lst in (paths, order):
            if gm not in lst:
                lst.insert(lst.index("bin/dc1/weapon-gameplay.bin") + 1, gm)
    # level-load screen without the Fireflies logo particle (the 2013 LOADING screen is the loading widget's own)
    ls = "bin/dc1/loadingscreen-settings.bin"
    dc_data_patches.patch_loading_screen(os.path.join(ROOT, "re", "gamebin", "dc1", "loadingscreen-settings.bin"),
                                         os.path.join(STAGE, *ls.split("/")))
    print("loading screen: Fireflies logo effect off (*loading-screen-settings* +8)")
    for lst in (paths, order):
        if ls not in lst:
            lst.insert(lst.index("bin/dc1/weapon-gameplay.bin") + 1, ls)
    # the Santa Barbara Abby clips (abby-prisoner-skel) on MP bodies: the three missing skeleton retarget pairs (dc_data_patches.patch_prisoner_retargets)
    if dc_data_patches.PARRY_PRISONER_SKEL_ON and os.environ.get("T2R_PRISONER_RETARGET", "1") != "0":
        sr = "bin/dc1/skel-retargets.bin"
        r = dc_data_patches.patch_prisoner_retargets(os.path.join(ROOT, "re", "gamebin", "dc1", "skel-retargets.bin"), os.path.join(STAGE, *sr.split("/")))
        print("skel retargets: abby-prisoner-skel -> " + ", ".join(f"{n:#x} x{s:.3f} (record {o:#x})" for o, n, s in r))
        for lst in (paths, order):
            if sr not in lst:
                lst.insert(lst.index("bin/dc1/weapon-gameplay.bin") + 1, sr)
    # Abby's guns held with Ellie grips (dc_data_patches.ABBY_GRIP_SWAPS); T2R_NO_ABBY_FIX=1 skips
    if os.environ.get("T2R_ABBY_GRIP_SWAP"):          # off: replaced Abby's animations (they looked cooked)
        ov = "bin/dc1/player-specific-weapon-overlays/abby-guns/"
        for fname, sw, kept in dc_data_patches.patch_abby_overlays(
                os.path.join(ROOT, "re", "gamebin", "dc1", "player-specific-weapon-overlays"),
                os.path.join(STAGE, *ov.split("/")), os.path.join(TOOLS, "win", "ext", "dcmain", "build", "sidbase.bin")):
            print(f"abby overlay {fname}: {sw} grip anims -> Ellie's ({kept} without an Ellie twin kept)")
            for lst in (paths, order):
                if ov + fname not in lst:
                    lst.insert(lst.index("bin/dc1/weapon-gameplay.bin") + 1, ov + fname)
    _prof.lap('listen mode, menu sounds, loading screen, abby overlays')
    # Backpack tab bar: the store (supplements) tab's icon is the pill svg; point the mod's widget
    # layout at the two-coin svg instead (same length, so patched in place)
    wp = os.path.join(STAGE, "actor97", "gui2-widgets.pak")
    wd = open(wp, "rb").read()
    n = wd.count(b"hud/player-menu/supplement-tab")
    open(wp, "wb").write(wd.replace(b"hud/player-menu/supplement-tab", b"hud/collectibles/coin-category"))
    print(f"gui2-widgets: supplements tab icon -> coin ({n} refs)")
    _prof.lap('gui2-widgets supplement-tab replace')
    import widgets_patch
    print(f"gui2-widgets: DETAILS panel on {widgets_patch.patch_widgets(wp)} weapon pickers")
    import loc_tables
    # every language table of the game (pol.common ...): the mod's text ids, the widget ciphers (static labels of our pages, loc_widgets via
    # widgets_patch) and the Polish rewrites of the patched recipe / prompt texts - after the widget patch (it fills the cipher registry)
    import loc_widgets
    loc_tables.finish_language_tables(STAGE, paths, order, os.environ.get("T2_GAME", "C:/Games/The Last of Us Part II Remastered"), loc_widgets.REGISTRY)
    _prof.lap('gui2 widgets_patch (DETAILS panel)')
    # the PC layout (gui2-widgets-pc.pak, game file from common.psarc, not in the mod) has its own
    # player menu with the same reference and wins on PC: shipped patched as a new mod file
    wd = open(os.path.join(ROOT, "re", "gamebin", "actor97", "gui2-widgets-pc.pak"), "rb").read()
    open(os.path.join(STAGE, "actor97", "gui2-widgets-pc.pak"), "wb").write(
        wd.replace(b"hud/player-menu/supplement-tab", b"hud/collectibles/coin-category"))
    import tabs_widgets
    print(f"gui2-widgets-pc: SP journal (collectibles) tab hidden in the TAB menu ({tabs_widgets.hide_journal_tab(os.path.join(STAGE, 'actor97', 'gui2-widgets-pc.pak'))} widgets)")
    for lst in (paths, order):
        if "actor97/gui2-widgets-pc.pak" not in lst:
            lst.insert(lst.index("actor97/gui2-widgets.pak") + 1, "actor97/gui2-widgets-pc.pak")
    _prof.lap('gui2-widgets-pc.pak patched copy')
    # Full Auto Rifle model: the MPX5 without its suppressor. The game's own pak, byte for byte,
    # with the can's triangles made degenerate in the index buffers (every LOD): offsets from
    # tools/models/mpx5_suppressor_faces.txt (made by the Noesis plugin strip rule, see
    # tools/models/README.md). Nothing else changes, so textures/UVs/normals stay as shipped
    # (a Noesis rebuild of the whole geometry looked off in game).
    # Now the can stays and is re-skinned to the silencer joint instead: hidden by the Full Auto
    # Rifle's default joints, shown on its suppressed variant (rifle-mpx5-supp).
    sys.path.insert(0, os.path.join(TOOLS, "models"))
    import ndpak, rebind
    mpak = ndpak.Pak(os.path.join(ROOT, "re", "models", "rifle-autorifle4.pak"))
    mp = bytearray(mpak.d)
    faces = []
    for line in open(os.path.join(ROOT, "tools", "models", "mpx5_suppressor_faces.txt")):
        off, first = map(int, line.split())
        assert struct.unpack_from("<H", mp, off)[0] == first, "MPX5 pak changed"
        faces.append(off)
    n = rebind.rebind_faces(mp, mpak, faces, dc_data_patches.FAR_CAN_JOINT)
    print(f"model: Full Auto Rifle can on the silencer joint ({n} vertices)")
    open(os.path.join(STAGE, "actor97", "rifle-autorifle4.pak"), "wb").write(mp)
    for lst in (paths, order):
        if "actor97/rifle-autorifle4.pak" not in lst:
            lst.insert(lst.index("actor97/gui2-widgets-pc.pak") + 1, "actor97/rifle-autorifle4.pak")
    _prof.lap('model: MPX5 can rebind')
    # Suppressed guns' own models (new actors, dc_data_patches.SUPP_MODELS): the gun's pak with
    # submeshes changed by the pak editor, listed in actor97.txt and loaded in every MP match
    cli = os.path.join(ROOT, "tlou2_pak_editor", "build", "gcc", "pak_editor_cli.exe")
    env = dict(os.environ, PATH=os.path.join(TOOLS, "win", "ext", "msys64", "ucrt64", "bin") + os.pathsep + os.environ["PATH"])
    for actor, (src, zero) in dc_data_patches.SUPP_MODELS.items():
        cur = os.path.join(ROOT, "re", "models", src)
        dst = os.path.join(STAGE, "actor97", actor + ".pak")
        for k, idx in enumerate(zero):
            out = dst + f".{k}"
            subprocess.run([cli, "zero", cur, out, "--index", str(idx)], check=True, env=env, capture_output=True)
            if cur != os.path.join(ROOT, "re", "models", src):
                os.remove(cur)
            cur = out
        shutil.move(cur, dst) if zero else shutil.copy(cur, dst)
        p = "actor97/" + actor + ".pak"
        for lst in (paths, order):
            if p not in lst:
                lst.insert(lst.index("actor97/rifle-autorifle4.pak") + 1, p)
    for gun, (src, joint, scale, (tx, ty, tz), pairs, ref, _) in dc_data_patches.CAN_GRAFTS.items():
        cur = os.path.join(ROOT, "re", "models", src)
        if gun == dc_data_patches.ENFORCER and deagle_pak:
            cur = deagle_pak
        colt_pak = None
        if gun == "pistol-beretta" and dc_data_patches.P1_COLT:
            # the 9mm = Part I's Colt Defender in the Beretta's pak (tools/part1/p1_guns.COLT, same actor / rig / anims);
            # the can is grafted onto it below (COLT_CAN_GRAFT). T2R_P1_COLT=0: the TLOU2 Beretta
            colt_pak = _colt_work_pak()
            cur = colt_pak
        canpak = os.path.join(ROOT, "re", "models", dc_data_patches.CAN_SOURCE)
        for k, (into, frm) in enumerate(pairs):
            out = os.path.join(STAGE, "actor97", f"{src}.{k}")
            r = subprocess.run([cli, "graft", cur, canpak, out, "--into", str(into), "--from", str(frm),
                                *dc_data_patches.CAN_SELECT, "--translate", str(tx), str(ty), str(tz),
                                "--joint", str(joint), "--ref", *map(str, ref), "--scale", str(scale)],
                               env=env, capture_output=True, text=True)
            if r.returncode:
                raise SystemExit(f"graft {gun} {into}<-{frm}: {r.stderr.strip()}")
            if k or cur == deagle_pak or cur == colt_pak:
                os.remove(cur)
            cur = out
        if gun == dc_data_patches.ENFORCER and deagle_pak:
            import rename_pak
            src = dc_data_patches.DEAGLE_ACTOR + ".pak"
            data, n = rename_pak.rename(open(cur, "rb").read(), "pistol-cz75-dina", dc_data_patches.DEAGLE_ACTOR)
            open(os.path.join(STAGE, "actor97", src), "wb").write(data)
            os.remove(cur)
            print(f"model: Desert Eagle renamed to {dc_data_patches.DEAGLE_ACTOR} ({n} names)")
        else:
            shutil.move(cur, os.path.join(STAGE, "actor97", src))
        p = "actor97/" + src
        for lst in (paths, order):
            if p not in lst:
                lst.insert(lst.index("actor97/rifle-autorifle4.pak") + 1, p)
        print(f"model: {gun} + suppressor can ({len(pairs)} grafts, joint {joint})")
    if dc_data_patches.FRONTIER_ON:
        # Suppressed Frontier Rifle's model: the game's Marlin pak + the MPX5's can on its body (per LOD), renamed to FRONTIER_SUPP_ACTOR
        fj, fscale, (ftx, fty, ftz), fpairs, fref, _ = dc_data_patches.FRONTIER_CAN_GRAFT
        fwork = os.path.join(TOOLS, "local", "_frontier_supp_work.pak")
        # the Marlin rests COCKED (the hammer reset after every shot): the hammer's vertices are pre-rotated by the cocked pose in the
        # pak itself (tools/models/frontier_hammer.py; the lever clips are baked for it), for the Frontier and its suppressed copy
        sys.path.insert(0, os.path.join(TOOLS, "models"))
        import frontier_hammer
        hrep = []
        marlin = frontier_hammer.patch_pak(_game_pak(dc_data_patches.FRONTIER_ACTOR), hrep)
        assert hrep and all(h[1] > 0 and h[2] == 0 for h in hrep), f"frontier hammer: unexpected patch report {hrep}"
        open(fwork, "wb").write(marlin)
        open(os.path.join(STAGE, "actor97", dc_data_patches.FRONTIER_ACTOR + ".pak"), "wb").write(marlin)
        p = "actor97/" + dc_data_patches.FRONTIER_ACTOR + ".pak"
        for lst in (paths, order):
            if p not in lst:
                lst.insert(lst.index("actor97/rifle-autorifle4.pak") + 1, p)
        print(f"model: Marlin hammer rests cocked ({sum(h[1] for h in hrep)} hammer vertices in {len(hrep)} LODs)")
        canpak = os.path.join(ROOT, "re", "models", dc_data_patches.CAN_SOURCE)
        cur = fwork
        for k, (into, frm) in enumerate(fpairs):
            out = fwork + f".g{k}"
            r = subprocess.run([cli, "graft", cur, canpak, out, "--into", str(into), "--from", str(frm),
                                *dc_data_patches.CAN_SELECT, "--translate", str(ftx), str(fty), str(ftz),
                                "--joint", str(fj), "--ref", *map(str, fref), "--scale", str(fscale)],
                               env=env, capture_output=True, text=True)
            if r.returncode:
                raise SystemExit(f"graft frontier {into}<-{frm}: {r.stderr.strip()}")
            os.remove(cur)
            cur = out
        sys.path.insert(0, os.path.join(TOOLS, "models"))
        import rename_pak
        fdata, fn = rename_pak.rename(open(cur, "rb").read(), dc_data_patches.FRONTIER_ACTOR, dc_data_patches.FRONTIER_SUPP_ACTOR)
        os.remove(cur)
        assert fn >= 7, f"frontier supp: only {fn} names renamed"
        open(os.path.join(STAGE, "actor97", dc_data_patches.FRONTIER_SUPP_ACTOR + ".pak"), "wb").write(fdata)
        p = "actor97/" + dc_data_patches.FRONTIER_SUPP_ACTOR + ".pak"
        for lst in (paths, order):
            if p not in lst:
                lst.insert(lst.index("actor97/rifle-autorifle4.pak") + 1, p)
        print(f"model: Frontier Rifle + suppressor can ({len(fpairs)} grafts, joint {fj}, {fn} names -> {dc_data_patches.FRONTIER_SUPP_ACTOR})")
    # Burst Rifle's can: grafted into the rifle-brst pak (built above; the base icon was rendered before this), per LOD like the Vepr's
    bj, bscale, (btx, bty, btz), bpairs, bref, _ = dc_data_patches.BURST_CAN_GRAFT
    bdst = os.path.join(STAGE, "actor97", dc_data_patches.BURST_ACTOR + ".pak")
    canpak = os.path.join(ROOT, "re", "models", dc_data_patches.CAN_SOURCE)
    cur = bdst
    for k, (into, frm) in enumerate(bpairs):
        out = bdst + f".g{k}"
        r = subprocess.run([cli, "graft", cur, canpak, out, "--into", str(into), "--from", str(frm),
                            *dc_data_patches.CAN_SELECT, "--translate", str(btx), str(bty), str(btz),
                            "--joint", str(bj), "--ref", *map(str, bref), "--scale", str(bscale)],
                           env=env, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(f"graft burst {into}<-{frm}: {r.stderr.strip()}")
        if cur != bdst:
            os.remove(cur)
        cur = out
    shutil.move(cur, bdst)
    print(f"model: Burst Rifle + suppressor can ({len(bpairs)} grafts, joint {bj})")
    _prof.lap('model: pak_editor_cli zero/graft (supp guns + cans)')
    # Suppressed Tactical Shotgun: its own actor = the black pump + a big can (dc_data_patches.TAC_CAN)
    tc = dc_data_patches.TAC_CAN
    cur = os.path.join(ROOT, "re", "models", tc["src"])
    if dc_data_patches.TAC_RIG_REMAP:
        # Tactical Shotgun on the player pump's rig (pump rack pop, re/tac-pump-pop.md): the game's
        # shotgun-pump-stock-black actor is replaced (only the Tac uses it in MP) and the suppressed
        # Tac below is grafted from the same remapped copy
        tpak = ndpak.Pak(cur)
        td = bytearray(tpak.d)
        moved = rebind.remap_joints(td, tpak, dc_data_patches.TAC_RIG_REMAP)
        assert all(moved.values()), f"Tac rig remap: nothing on {moved} (pak changed?)"
        cur = os.path.join(STAGE, "actor97", tc["src"])
        open(cur, "wb").write(td)
        p = "actor97/" + tc["src"]
        for lst in (paths, order):
            if p not in lst:
                lst.insert(lst.index("actor97/rifle-autorifle4.pak") + 1, p)
        print(f"model: Tactical Shotgun on the player pump rig (influences moved {moved})")
    dst = os.path.join(STAGE, "actor97", dc_data_patches.TAC_SUPP_ACTOR + ".pak")
    canpak = os.path.join(ROOT, "re", "models", dc_data_patches.CAN_SOURCE)
    for k, (into, frm) in enumerate(tc["pairs"]):
        out = dst + f".{k}"
        r = subprocess.run([cli, "graft", cur, canpak, out, "--into", str(into), "--from", str(frm),
                            *dc_data_patches.CAN_SELECT, "--translate", *map(str, tc["translate"]),
                            "--joint", str(tc["joint"]), "--ref", *map(str, tc["ref"]), "--scale", str(tc["scale"])],
                           env=env, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(f"graft tactical shotgun {into}<-{frm}: {r.stderr.strip()}")
        if k:
            os.remove(cur)
        cur = out
    sys.path.insert(0, os.path.join(TOOLS, "models"))
    import rename_pak
    data, n = rename_pak.rename(open(cur, "rb").read(), "shotgun-pump-stock-black", dc_data_patches.TAC_SUPP_ACTOR)
    open(dst, "wb").write(data)
    os.remove(cur)
    p = "actor97/" + dc_data_patches.TAC_SUPP_ACTOR + ".pak"
    for lst in (paths, order):
        if p not in lst:
            lst.insert(lst.index("actor97/rifle-autorifle4.pak") + 1, p)
    print("model: suppressed Tactical Shotgun (big can)")
    _prof.lap('model: suppressed Tactical Shotgun graft')
    if UC4_DOWNED:
        stage_uc4_downed(paths, order)
    _prof.lap('stage_uc4_downed (UC4 downed anim paks)')
    if GUNEXEC:
        stage_gunexec(paths, order)
        if GX_WEAPON:
            stage_gx_weapon(paths, order)
    if os.environ.get("T2R_EXEC_RECOIL", "1") != "0":
        stage_exec_recoil(paths, order)
    _prof.lap('stage_gunexec + stage_gx_weapon (gun execution carriers) + stage_exec_recoil')
    stage_plant_anim(paths, order)
    _prof.lap('stage_plant_anim (placed bomb setting clip)')
    stage_parry_anim(paths, order)
    _prof.lap('stage_parry_anim (duel parry clips)')
    stage_molo_death(paths, order)
    _prof.lap('stage_molo_death (SP burning-death clips)')
    stage_unstream_disarms(paths, order)
    _prof.lap('stage_unstream_disarms (SP hatchet disarm clips made resident)')
    stage_mp_snow(paths, order)
    stage_crew_winter(paths, order)
    _prof.lap('stage_crew_winter (Owen / Manny / Jordan winter parts, T2R_MP_SNOW=1)')
    stage_winter_under(paths, order)
    _prof.lap('stage_winter_under (pushed copies of the WLF winter clothes for the armoured winter twins)')
    stage_scar_gloves(paths, order)
    _prof.lap('stage_scar_gloves (Seraphite winter leather gloves, T2R_MP_SNOW=1)')
    stage_scar_female_winter(paths, order)
    _prof.lap('stage_scar_female_winter (female Seraphite winter mask + gloves, T2R_MP_SNOW=1)')
    _prof.lap('stage_mp_snow (snow-capable garment material, T2R_MP_SNOW=1)')
    stage_glint_part(paths, order)
    _prof.lap('stage_glint_part (scope glint particle module)')
    dc_data_patches.EXTRA_ACTORS[:] = list(dc_data_patches.SUPP_MODELS) + [dc_data_patches.TAC_SUPP_ACTOR, dc_data_patches.BURST_ACTOR] + ([dc_data_patches.FRONTIER_SUPP_ACTOR] if dc_data_patches.FRONTIER_ON else []) + ([dc_data_patches.MILSNIPER_ACTOR] if dc_data_patches.MILSNIPER_ON else []) +         ([dc_data_patches.DEAGLE_ACTOR] if DEAGLE else []) + ([dc_data_patches.DIABLO_ACTOR, dc_data_patches.SHORTY_ACTOR] if dc_data_patches.FACTION_GUNS else []) +         [dc_data_patches.ARMOUR_HELMET_ACTOR, dc_data_patches.ARMOUR_TORSO_ACTOR, dc_data_patches.SATCHEL_ACTOR] + dc_data_patches.HELD_BAG_ACTORS + (dc_data_patches.JOEL_MM_ACTORS if dc_data_patches.JOEL_MM_ON else []) + \
        (dc_data_patches.WINTER_ACTORS if dc_data_patches.WINTER_ON else []) + (dc_data_patches.WINTER_GLOVE_ACTORS if dc_data_patches.WINTER_GLOVES_ON else []) + dc_data_patches.WINTER_FSCAR_ACTORS + [dc_data_patches.ARMOUR_RIG_VEST_ACTOR] +         (list(dc_data_patches.BACKPACK_RIG.values()) if dc_data_patches.BACKPACK_RIG_ON else []) + \
        (list(dc_data_patches.BACKPACK_PROPS.values()) if dc_data_patches.BACKPACK_PROPS_ON else []) + \
        (dc_data_patches.ARMOUR_UNDER_COPIES if dc_data_patches.ARMOUR_UNDER_ON else []) +         (dc_data_patches.ARMOUR_FEMALE_ACTORS + (dc_data_patches.ARMOUR_UNDER_COPIES_F if dc_data_patches.ARMOUR_UNDER_ON else []) if dc_data_patches.ARMOUR_RIG_FEMALE_ON else [])
    if dc_data_patches.WINTER_ON and dc_data_patches.ARMOUR_UNDER_ON and dc_data_patches.ARMOUR_RIG_FEMALE_ON:      # stage_winter_under's copies
        dc_data_patches.EXTRA_ACTORS += [x for x in dc_data_patches.WINTER_UNDER_COPIES if x not in dc_data_patches.EXTRA_ACTORS]
    dc_data_patches.patch_actor_list(os.path.join(STAGE, "actor97.txt"))
    _prof.lap('actor97.txt patch')
    # Map thumbnails showed "?": the game picks <name>_1080p.png / <name>_1440p.png by screen
    # resolution (its own streaming/menus images all ship the 3), and the mod only had <name>.png.
    # The mod blob must stay inside the DLL's original resource (a bigger one is appended as a new
    # section, doubling the DLL, and Defender flagged that build), so all three are made smaller
    # than the mod's 1920x1080 original: 1280x720 base, 1066x600 and 800x450 (the thumbnail is
    # drawn far smaller than that). The unused blockmesh image is left alone.
    from PIL import Image
    tdir = os.path.join(STAGE, "gtex1", "streaming", "menus", "to1")
    hosp = os.path.join(tdir, "map-pvp-hospital.png")   # Hospital had no thumbnail of its own (showed Forest)
    # the game's own No Return Hospital polaroid (rl-map-locale-hospital-color in actor97/rogue-map-photo-kit.pak),
    # extracted with models/texdict.py and framed 1280x720; the generated mockup below is only the fallback
    hosp_src = os.path.join(TOOLS, "features", "assets", "map-pvp-hospital.png")
    if not os.path.exists(hosp) and os.path.isdir(tdir) and os.path.exists(hosp_src):
        shutil.copyfile(hosp_src, hosp)
    if not os.path.exists(hosp) and os.path.isdir(tdir):
        from PIL import ImageDraw, ImageFont
        himg = Image.new("RGB", (1280, 720), (34, 44, 48))
        d = ImageDraw.Draw(himg)
        for y in range(720):
            d.line([(0, y), (1280, y)], fill=(30 + y // 14, 42 + y // 16, 46 + y // 18))
        d.rectangle([380, 180, 900, 560], outline=(190, 200, 195), width=8)
        d.rectangle([600, 270, 680, 470], fill=(200, 60, 55)); d.rectangle([540, 330, 740, 410], fill=(200, 60, 55))
        try: f = ImageFont.truetype("arialbd.ttf", 96)
        except Exception: f = ImageFont.load_default()
        d.text((640, 630), "HOSPITAL", fill=(235, 235, 230), font=f, anchor="mm")
        himg.save(hosp)
    # SP-section maps (T2R_SP_MAPS): tools/features/assets/map-pvp-<key>.png when there is one, else a name card
    for m in sp_maps_build.selected():
        if m.get("replaces"):       # the Hospital keeps its own card (above)
            continue
        img = os.path.join(tdir, f"map-pvp-{m['key']}.png")
        asset = os.path.join(TOOLS, "features", "assets", f"map-pvp-{m['key']}.png")
        if os.path.exists(img) or not os.path.isdir(tdir):
            continue
        if os.path.exists(asset):
            shutil.copyfile(asset, img)
            continue
        from PIL import ImageDraw, ImageFont
        card = Image.new("RGB", (1280, 720), (34, 44, 48))
        d = ImageDraw.Draw(card)
        for y in range(720):
            d.line([(0, y), (1280, y)], fill=(30 + y // 14, 42 + y // 16, 46 + y // 18))
        try: f = ImageFont.truetype("arialbd.ttf", 96)
        except Exception: f = ImageFont.load_default()
        d.text((640, 360), m["name"].upper(), fill=(235, 235, 230), font=f, anchor="mm")
        card.save(img)
    for src in sorted(glob.glob(os.path.join(tdir, "map-pvp-*.png"))):
        if src.endswith(("_1080p.png", "_1440p.png")) or "blockmesh" in src:
            continue
        sizes = (("", (1280, 720)), ("_1440p", (1066, 600)), ("_1080p", (800, 450)))

        def make_thumbs():
            im = Image.open(src).convert("RGB")
            for suf, size in sizes:
                im.resize(size, Image.LANCZOS).save(src[:-4] + suf + ".png", optimize=True)
        _bc.cached("map-thumb", [_bc.File(src), sizes, getattr(Image, "__version__", "")], make_thumbs,
                   files=[src[:-4] + suf + ".png" for suf, _ in sizes])
        for suf, size in sizes:
            dst = src[:-4] + suf + ".png"
            p = "gtex1/streaming/menus/to1/" + os.path.basename(dst)
            for lst in (paths, order):
                if p not in lst:
                    last = max(i for i, x in enumerate(lst) if x.startswith("gtex1/"))
                    lst.insert(last + 1, p)
    _prof.lap('map thumbnails (PIL resize x3)')
    # build salt (unreferenced file): the blob is LZ4-compressed, so any change reshuffles the
    # DLL's bytes and Windows Defender's ML sometimes quarantines it (Program:Win32/Contebrew.A!ml,
    # a false positive); main() then bumps the salt and rebuilds until a scan is clean
    open(os.path.join(STAGE, "t2r-salt.txt"), "w").write(f"t2r build salt {salt}\n")
    for lst in (paths, order):
        if "t2r-salt.txt" not in lst:
            lst.append("t2r-salt.txt")
    with open(man, "w", newline="\n") as f:
        f.write("\n".join(lines[:1] + [p for p in paths if p] + ["--order--"] + [o for o in order if o]) + "\n")


def _game_pak(name):
    """actor97/<name>.pak of the game (cached in re/gamebin/actor97, else read out of sp-common.psarc)"""
    src = os.path.join(ROOT, "re", "gamebin", "actor97", name + ".pak")
    if not os.path.exists(src):
        import dsar_list
        game = os.environ.get("T2_GAME", "C:/Games/The Last of Us Part II Remastered")
        d, ent, bs, bsz = dsar_list.open_files(os.path.join(game, "build", "pc", "main", "sp-common.psarc"))
        os.makedirs(os.path.dirname(src), exist_ok=True)
        open(src, "wb").write(dsar_list.extract(d, ent, bs, bsz, "actor97/" + name + ".pak"))
    return open(src, "rb").read()


def _game_bank(name, archive):
    """soundbank4/<name>.bnk of the game, unchanged (cached in re/sound, else read out of build/pc/main/<archive>)"""
    src = os.path.join(ROOT, "re", "sound", name + ".bnk")
    if not os.path.exists(src):
        import dsar_list
        game = os.environ.get("T2_GAME", "C:/Games/The Last of Us Part II Remastered")
        d, ent, bs, bsz = dsar_list.open_files(os.path.join(game, "build", "pc", "main", archive))
        os.makedirs(os.path.dirname(src), exist_ok=True)
        open(src, "wb").write(dsar_list.extract(d, ent, bs, bsz, "soundbank4/" + name + ".bnk"))
    data = open(src, "rb").read()
    assert data[0x20:0x24] == b"SBlk", f"{name}.bnk: not a sound bank"
    return data


def stage_assault(paths, order):
    """2013 Assault Rifle = the game's own No Return rifle (dc_data_patches.ASSAULT): ships its two assets that live in world-rogue.psarc, an
    archive an MP match does not mount - actor97/rifle-assault-firefly.pak (the M4 model, textures inside; the game's pak unchanged, under the
    game's own actor name, the level lists it) and soundbank4/wpn-rifle-m4.bnk (shot / dry fire / reload foley; the pak's SOUND_BANK_TABLE and
    the level's sound-bank line both name it).  Cached in re/gamebin / re/sound like the other game files."""
    D = dc_data_patches
    pak = D.ASSAULT_ACTOR
    src = os.path.join(ROOT, "re", "gamebin", "actor97", pak + ".pak")
    if not os.path.exists(src):
        import dsar_list
        game = os.environ.get("T2_GAME", "C:/Games/The Last of Us Part II Remastered")
        d, ent, bs, bsz = dsar_list.open_files(os.path.join(game, "build", "pc", "main", D.ASSAULT_ARCHIVE))
        os.makedirs(os.path.dirname(src), exist_ok=True)
        open(src, "wb").write(dsar_list.extract(d, ent, bs, bsz, "actor97/" + pak + ".pak"))
    data = open(src, "rb").read()
    if b"SOUND_BANK_TABLE" not in data or D.ASSAULT_BANK.encode() not in data:
        raise SystemExit(f"{pak}.pak: no SOUND_BANK_TABLE naming {D.ASSAULT_BANK} (not the game's No Return rifle pak?)")
    os.makedirs(os.path.join(STAGE, "actor97"), exist_ok=True)
    open(os.path.join(STAGE, "actor97", pak + ".pak"), "wb").write(data)
    p = "actor97/" + pak + ".pak"
    for lst in (paths, order):
        if p not in lst:
            anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
            lst.insert(lst.index(anchor) + 1, p)
    bank = _game_bank(D.ASSAULT_BANK, D.ASSAULT_ARCHIVE)
    inner = bytes(bank[0x40:0xa0]).split(b"\0")[0].decode()
    assert inner == D.ASSAULT_BANK, f"{D.ASSAULT_BANK}.bnk carries the inner name {inner}"
    os.makedirs(os.path.join(STAGE, "soundbank4"), exist_ok=True)
    open(os.path.join(STAGE, "soundbank4", D.ASSAULT_BANK + ".bnk"), "wb").write(bank)
    for lst in (paths, order):
        if f"soundbank4/{D.ASSAULT_BANK}.bnk" not in lst:
            last = max(i for i, x in enumerate(lst) if x.startswith("gtex1/") or x.startswith("soundbank4/"))
            lst.insert(last + 1, f"soundbank4/{D.ASSAULT_BANK}.bnk")
    print(f"assault: actor {pak}.pak ({len(data)} bytes) + bank {D.ASSAULT_BANK}.bnk ({len(bank)} bytes) shipped from {D.ASSAULT_ARCHIVE}")


def _strip_effects(name, data):
    # the carriers keep their ORIGINAL clips' anim effect tables (ANIM +0x58 -> {clip sid, events *, u32 count
    # at +0x10}): the clicker-leash clips fired chain-yank sounds every few seconds while crawling,
    # the workbench ones revolver foley, the horse ones hit sounds. Our baked motion has nothing to do with them:
    # every ANIM's event count -> 0 (the table stays, nothing fires).
    import pk
    tmp = os.path.join(TOOLS, "local", "_fx_strip.pak")
    open(tmp, "wb").write(data)
    p = pk.Pak(tmp)
    d = bytearray(data)
    n = ev = 0
    for r in p.res:
        if r["type"] != "ANIM":
            continue
        t = p.ptr(r["start"] + r["off"] + 0x58)
        if t is None:
            continue
        c = struct.unpack_from("<I", d, t + 0x10)[0]
        if c:
            struct.pack_into("<I", d, t + 0x10, 0)
            n += 1
            ev += c
    os.remove(tmp)
    print(f"anim: {name} - anim effect events stripped from {n} clips ({ev} events)")
    return bytes(d)


def _stage_anim_pak(paths, order, name, data, what):
    data = _strip_effects(name, data)
    open(os.path.join(STAGE, "actor97", name + ".pak"), "wb").write(data)
    p = "actor97/" + name + ".pak"
    for lst in (paths, order):
        if p not in lst:
            lst.insert(lst.index("actor97/rifle-autorifle4.pak") + 1, p)
    print(f"anim: {what} pak {name} ({len(data)} bytes) in the MP level")


def stage_molo_death(paths, order):
    """actor97/anim-npc-m-cover-share.pak with SP's burning human death (npc-m-death-molotov-{fw-a,bw,lt-a,rt-a}) baked into four of its carriers
    (molo_death_pak.py; deathreact.c DR_CLIPS[0][2] plays them by the carrier names). Listed in the MP level by dc_data_patches.MP_LEVEL_ADDS
    (T2R_MOLO_DEATH_PAK=0: not built, the Ellie fire death plays). The full anim-npc-m.pak stays out (crash)."""
    if not dc_data_patches.MOLO_DEATH_ON:
        return
    import molo_death_pak
    src = _game_pak(molo_death_pak.SRC_PAK)
    # the host is the pak AS STAGED: stage_gunexec already put the second carrier set (revive / revolver / machete clips, build_gunexec_pak.PAK_SETS) into this
    # very file, and starting from the game's copy replaced it with the unpatched pak (revive / those executions played the carriers' own cover clips). The four
    # molotov carriers are kept out of that set's pool (build_gunexec_pak.MOLO_CARRIERS), so patching them in place leaves every baked clip alone.
    staged = os.path.join(STAGE, "actor97", molo_death_pak.HOST_PAK + ".pak")
    host = open(staged, "rb").read() if os.path.exists(staged) else _game_pak(molo_death_pak.HOST_PAK)
    if os.path.exists(staged):
        print(f"molo death: host = the staged {molo_death_pak.HOST_PAK}.pak (gun execution / revive carriers kept)")
    data = molo_death_pak.build(src, host)
    molo_death_pak.verify(src, host, data)
    _stage_anim_pak(paths, order, molo_death_pak.HOST_PAK, data, "SP burning death (molotov)")


def stage_unstream_disarms(paths, order):
    """actor97/anim-melee-abby-t2.pak (the game's own Abby melee pak, already loaded by every MP match through abby-player-fundamentals) with SP's
    hatchet disarm clips made RESIDENT (tools/features/unstream_anims.py: the streamed clips' chunks decoded + re-encoded into full ClipData on the
    pak's last data page, ANIM streamed bit cleared). ctxdown.c plays them by their own names (mduel_fin.h mf-fist-1h-blade-disarm-iron-rhino /
    -punch-n-hack). NOT _stage_anim_pak: that strips every clip's effect events, and this pak's clips need theirs. T2R_UNSTREAM_DISARMS=0: not staged
    (the game's pak loads; the ext's T2R_DIS_HATCHET guard file t2r-duel-no-hatchet-dis.txt then keeps the borrowed rows)."""
    if os.environ.get("T2R_UNSTREAM_DISARMS", "1") == "0":
        return
    import unstream_anims
    src = _game_pak(unstream_anims.PAK)
    out = os.path.join(STAGE, "actor97", unstream_anims.PAK + ".pak")

    def make():
        data, info = unstream_anims.build(src)
        if not unstream_anims.verify(data, src):
            raise SystemExit(f"unstream: {unstream_anims.PAK} does not decode back to the source clips")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        open(out, "wb").write(data)
        return info
    _bc.cached("unstream-" + unstream_anims.PAK, [_bc.Code("unstream_anims"), _env_scoped("unstream_anims"), src, list(unstream_anims.CLIPS)], make, files=[out])
    p = "actor97/" + unstream_anims.PAK + ".pak"
    for lst in (paths, order):
        if p not in lst:
            lst.insert(lst.index("actor97/rifle-autorifle4.pak") + 1, p)
    print(f"anim: {unstream_anims.PAK} with {len(unstream_anims.CLIPS)} SP disarm clips made resident ({os.path.getsize(out)} bytes)")


def _snow_build(data, g, donor, report, donor_pak, mask):
    """mp_snow_pak.build, run in a child interpreter when the mpsnow steps run in parallel (the snow masks are CPU-bound python, so threads alone do not
    help: the pool threads wait on children). Exceptions the callers handle are re-raised as the same types; T2R_PAR_JOBS=1 = the direct call."""
    import mp_snow_pak
    import parjobs
    if parjobs.workers_default(4) <= 1:
        return mp_snow_pak.build(data, g, donor=donor, report=report, donor_pak=donor_pak, mask=mask)
    r = parjobs.run([("winter_jobs", "snow_build", (data, g, donor, donor_pak, mask), {})], force_child=True)[0]
    if r[0] == "ok":
        report.update(r[2])
        return r[1]
    raise {"cli": mp_snow_pak.CliFailed, "exit": SystemExit, "stop": StopIteration, "key": KeyError}[r[0]](r[1])


def _pool_calls(calls, cap=4):
    """run the zero-argument callables on threads (independent cached steps: their time is in pak_editor_cli child processes), results in order.
    A pak_editor_cli crash under load (night50: 0xc0000142) is retried once per call; any other error is raised after the others finished.
    Width: T2R_PAR_JOBS or min(cpu - 2, cap); 1 = serial."""
    import concurrent.futures
    import parjobs
    w = parjobs.workers_default(cap)

    def one(c):
        try:
            return c()
        except SystemExit as e:
            if "pak_editor_cli failed" not in str(e) and e.__class__.__name__ != "CliFailed":
                raise
            print(f"build: {e!s:.100} - retrying once")
            return c()
    if w <= 1 or len(calls) <= 1:
        return [one(c) for c in calls]
    with concurrent.futures.ThreadPoolExecutor(max_workers=w) as ex:
        futs = [ex.submit(one, c) for c in calls]
    return [f.result() for f in futs]


def stage_mp_snow(paths, order):
    """actor97/<kit>.pak of mp_snow.PAKS with the garment's material rebuilt on the snow-capable wlf-patch technique and its tint baked into the
    colour texture (tools/models/mp_snow_pak.py; T2R_MP_SNOW=1 only). The kit is already in the MP level (the pak keeps its name), so it is only
    replaced, not listed (a pak that was not in the level, e.g. militia-male-01-kit, would need an MP_LEVEL_ADDS entry too)."""
    if not mp_snow.ON:
        return
    sys.path.insert(0, os.path.join(TOOLS, "models"))
    import mp_snow_pak
    kits = []
    for kit, garment, mdir in mp_snow.ALL:
        if kit not in kits:
            kits.append(kit)
    todo_kits = []
    for kit in kits:
        gs = [(g, d) for k, g, d in mp_snow.ALL if k == kit]
        dpak = None if kit in mp_snow.DONOR_KITS else _game_pak(mp_snow.DONOR_PAK)      # (no wlf-patch in this pak: graft it, cross-pak test)
        src = _game_pak(kit)
        for g, d in gs:
            if (d + g + ":default").encode() not in src:
                raise SystemExit(f"mp_snow: {kit}.pak has no material {d}{g}:default (the table sid would not match)")
        out = os.path.join(STAGE, "actor97", kit + ".pak")

        def make(kit=kit, gs=gs, src=src, out=out, dpak=dpak):
            reps = {}
            data = src
            for g, _ in gs:                              # one garment after the other: each build starts from the previous result
                rep = {}
                dn, dpk = "wlf-patch", dpak
                if (kit, g) in mp_snow.DONOR_OF:        # material swap: the closest SP winter garment of its type is the donor (mp_snow.SWAP)
                    dpk_name, dn, _prm = mp_snow.DONOR_OF[(kit, g)]
                    dpk = _game_pak(dpk_name)
                try:
                    data = _snow_build(data, g, dn, rep, dpk, (kit, g) in mp_snow.DONOR_OF)      # swap garments get their own baked snow patch map
                except SystemExit as e:
                    if "pak_editor_cli failed" in str(e):      # a tool crash (night50: 0xc0000142 under load) is not a data verdict: never cache the garment as skipped
                        raise
                    print(f"mp snow: {kit}/{g} skipped ({str(e)[:120]})")
                    continue
                if rep.get("readback_rmse_flipped") is None or rep["readback_rmse_flipped"] > 4:
                    raise SystemExit(f"mp_snow: {kit}/{g}: the injected colour texture does not read back ({rep.get('readback_rmse_flipped')})")
                reps[g] = rep
            os.makedirs(os.path.dirname(out), exist_ok=True)
            open(out, "wb").write(data)
            return reps
        todo_kits.append((kit, gs, out, lambda kit=kit, gs=gs, src=src, dpak=dpak, make=make, out=out: _bc.cached("mpsnow-" + kit, [_bc.Code("mp_snow_pak"), _bc.Code("snow_clone"), _bc.Code("snow_mask"), _env_scoped("mp_snow_pak", "snow_clone"), _snow_inputs(), src, repr(gs), repr([mp_snow.DONOR_OF.get((kit, g)) for g, _ in gs]), dpak or b""], make, files=[out])))
    _pool_calls([c for _k, _g, _o, c in todo_kits])          # (the kits are independent: own source pak, own output; paths / prints below in the original order)
    for kit, gs, out, _c in todo_kits:
        p = "actor97/" + kit + ".pak"
        for lst in (paths, order):
            if p not in lst:
                lst.insert(lst.index("actor97/rifle-autorifle4.pak") + 1, p)
        print(f"mp snow: {kit}.pak with {', '.join(g for g, _ in gs)} on the snow technique ({os.path.getsize(out)} bytes)")
    if mp_snow.XPAK_ON:
        stage_mp_snow_copies(paths, kits)


def stage_mp_snow_copies(paths, kits):
    """(the Scar leather backpack flap had no snow): the mod's own copies of mp_snow.SWAP garments - armour twins (-arm1- / -farm01-),
    rigged look bags (-t2r-bp-), TAB bag props (-t2r-tab-bprop) - are built from the GAME paks before stage_mp_snow and kept the no-snow material.
    Each staged copy that carries a SWAP garment material (same path, so the same table row) gets the same material swap + its own baked snow map.
    Garments already on a snow technique are left alone (idempotent when an earlier step restored its output from the cache)."""
    import re
    import mp_snow_pak
    import pak_graft
    import snow_clone
    done = []
    todo = []
    for p in list(paths):
        kit = p[len("actor97/"):-len(".pak")] if p.startswith("actor97/") and p.endswith(".pak") else None
        if not kit or kit in kits or not re.search(mp_snow.COPY_RE, kit):
            continue
        f = os.path.join(STAGE, p)
        if not os.path.isfile(f):
            continue
        src = open(f, "rb").read()
        rows = list(dict.fromkeys((g, d, t) for _k, g, d, t in list(mp_snow.SWAP) + [("", *r) for r in mp_snow.swap_gear_rows()] if (d + g + ":default").encode() in src))
        if not rows:
            continue
        tech = snow_clone.techniques(snow_clone.Pak(src))
        rows = [r for r in rows if r[0] in tech and not any(x["name"].startswith("g_snowStrength") for x in snow_clone.read_rec(snow_clone.Pak(src), tech[r[0]])[0])]
        if not rows:
            continue

        def make(src=src, rows=rows, f=f, kit=kit):
            data, rep = src, {}
            for g, _d, ty in rows:
                if pak_graft.is_prop_pak(snow_clone.Pak(data)) and ty == "coat":
                    ty = "coat-prop"                    # (copies of the -jacket-cloth props: the prop-safe donor, mp_snow.DONOR_TYPES)
                dpk_name, dn, _prm = mp_snow.DONOR_TYPES[ty]
                try:
                    data = _snow_build(data, g, dn, {}, _game_pak(dpk_name), True)
                    rep[g] = "ok"
                except mp_snow_pak.CliFailed:
                    raise                               # (the tool failed, not the garment: never cached as a skip - night42 shipped every backpack copy without snow)
                except (SystemExit, StopIteration, KeyError) as e:
                    rep[g] = "skipped: " + repr(e)[:100]
            open(f, "wb").write(data)
            return rep
        todo.append((kit, lambda kit=kit, src=src, rows=rows, make=make, f=f: _bc.cached("mpsnow-copy-" + kit, [_bc.Code("mp_snow_pak"), _bc.Code("snow_clone"), _bc.Code("pak_graft"), _bc.Code("snow_mask"), _env_scoped("mp_snow_pak", "snow_clone"),
                                                                                         _snow_inputs(), src, repr(rows)], make, files=[f])))
    for (kit, _c), rep in zip(todo, _pool_calls([c for _k, c in todo])):          # (independent copies: own staged file each; report lines in the original order)
        done.append(f"{kit} ({', '.join(f'{g} {v}' for g, v in rep.items())})")
    print(f"mp snow: mod copies of swap garments: {'; '.join(done) or 'none'}")


def stage_crew_winter(paths, order):
    """actor97/<part>.pak of dc_data_patches.WINTER_LOOKS: the SP tracking outfits of Owen / Manny / Jordan (clothes only, retargeted to base-male-skel, LOD0 only,
    backpack / holster cut: tools/models/crew_winter.py), the shrink-wrapped Seraphite half masks (scar_mask.py), the SP Jackson winter torsos (winter_gear.py).
    New paks of the mod, listed in the level by dc_data_patches.MP_LEVEL_ADDS; the heads come from level-added sp-common kits (WINTER_KIT_ACTORS).
    Verifies that every part of every winter look resolves (night19: parts that did not resolve rendered as nothing)."""
    if not dc_data_patches.WINTER_ON:
        return
    sys.path.insert(0, os.path.join(TOOLS, "models"))
    import crew_winter
    import scar_mask
    import winter_gear
    assert (crew_winter.mod_names("owen") + crew_winter.mod_names("manny") + crew_winter.mod_names("jordan") if dc_data_patches.WINTER_CREW_ON else []) + \
        [scar_mask.actor_of(g) for g in scar_mask.GROUPS] + [g for g, _a in winter_gear.GEAR] == dc_data_patches.WINTER_ACTORS and \
        {g: scar_mask.part_of(g) for g in scar_mask.GROUPS} == dc_data_patches.WINTER_MASK_PARTS, \
        "crew_winter / scar_mask / winter_gear != dc_data_patches.WINTER_LOOKS"
    outs = [os.path.join(STAGE, "actor97", a + ".pak") for a in dc_data_patches.WINTER_ACTORS]

    def make():
        # the crews, mask groups and the gear paks are independent (each writes its own paks): child interpreters in parallel (parjobs.py; T2R_PAR_JOBS=1 = serial,
        # same bytes); the report is merged in the serial order
        import parjobs
        out97 = os.path.join(STAGE, "actor97")
        os.makedirs(out97, exist_ok=True)
        jobs = []
        if dc_data_patches.WINTER_CREW_ON:
            jobs += [("winter_jobs", "crew", (out97, c), {}) for c in crew_winter.CREW]
        jobs += [("winter_jobs", "mask", (out97, g), {}) for g in scar_mask.GROUPS]
        jobs.append(("winter_jobs", "gear", (out97,), {}))
        rep = {}
        for r in parjobs.run(jobs, workers=parjobs.workers_default(4)):
            rep.update(r)
        return rep
    _bc.cached("crew-winter", [_bc.Code("crew_winter"), _bc.Code("crew_retarget"), _bc.Code("rename_pak"), _bc.Code("scar_mask"), _bc.Code("winter_gear"), _bc.Code("mp_snow_pak"),
                                _env_scoped("crew_winter", "crew_retarget", "rename_pak", "scar_mask", "winter_gear", "mp_snow_pak"), _snow_inputs()],
               make, files=outs)
    import armour_paks
    import armour_rig
    geo_of = {}
    for a in dc_data_patches.WINTER_ACTORS:
        pk = armour_paks._load(open(os.path.join(STAGE, "actor97", a + ".pak"), "rb").read())
        geo_of[a] = {n[4:].split(".")[0] for n, t, _pi, _io in pk.items if t == "FOREGROUND_SECTION_2" and n.startswith("geo.")}
        ids = {h.hex() for h in armour_rig.hier_ids(pk)}
        want = winter_gear.HIER["female" if "-female-" in a else "male"]
        assert ids == {want}, f"{a}: hierarchy ids {ids}, base skeleton {want}"
    parts_staged = {pt for gs in geo_of.values() for pt in gs}
    kit_adds = {k for k in dc_data_patches.WINTER_KIT_ACTORS}
    for look, (base, parts, mode) in dc_data_patches.WINTER_LOOKS.items():
        for part in parts:
            if part in parts_staged or (dc_data_patches.WINTER_FSCAR_ON and part == dc_data_patches.WINTER_FSCAR_MASK[1]):      # (female mask: stage_scar_female_winter checks it)
                continue
            kit = part.replace("-head-", "-") + "-kit"
            assert kit in kit_adds and ("actor " + kit) in dc_data_patches.MP_LEVEL_ADDS and os.path.exists(armour_rig.ref_pak(kit)), \
                f"winter look {look}: part {part} is neither a geo part of a staged pak nor the head of a level-added sp-common kit"
    for a in dc_data_patches.WINTER_ACTORS:
        assert "actor " + a in dc_data_patches.MP_LEVEL_ADDS, f"{a}: staged but not in MP_LEVEL_ADDS"
        p = "actor97/" + a + ".pak"
        for lst in (paths, order):
            if p not in lst:
                anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
                lst.insert(lst.index(anchor) + 1, p)
    print(f"model: winter parts {', '.join(dc_data_patches.WINTER_ACTORS)} ({sum(os.path.getsize(o) for o in outs)} bytes), heads of {', '.join(dc_data_patches.WINTER_KIT_ACTORS)}; all "
          f"{sum(len(v[1]) for v in dc_data_patches.WINTER_LOOKS.values())} look parts resolve")


def stage_winter_under(paths, order):
    """actor97/<copy>.pak of dc_data_patches.WINTER_UNDER_COPIES (T2R_MP_SNOW=1 + armour twins): the WLF winter looks' own clothes (Jackson torsos, crew outfits, beanie
    heads) pushed under the Rattler vest / helmet, so the armour does not clip through them (tools/models/armour_winter_push.py; t2r-ext ARMOR_WINTER_BODY swaps them in)."""
    if not (dc_data_patches.WINTER_ON and dc_data_patches.ARMOUR_UNDER_ON and dc_data_patches.ARMOUR_RIG_FEMALE_ON):
        return
    import armour_female
    import armour_rattler
    import armour_winter_push
    adir = os.path.join(STAGE, "actor97")
    assert sorted(armour_winter_push.pushed(a) for a in armour_winter_push.SOURCES) == sorted(dc_data_patches.WINTER_UNDER_COPIES) and         {p for v in armour_winter_push.SOURCES.values() for p in v[0]} == dc_data_patches.WINTER_UNDER_PARTS, "armour_winter_push != dc_data_patches.WINTER_UNDER_*"
    outs = [os.path.join(adir, a + ".pak") for a in dc_data_patches.WINTER_UNDER_COPIES]
    srcs = [armour_winter_push.source_pak(s, adir) for s in armour_winter_push.SOURCES]

    def make():
        rep = {}
        armour_winter_push.build(adir, adir, armour_rattler.build(adir), armour_female.build(adir), report=rep)
        return rep
    _bc.cached("winter-under", [_bc.Code("armour_winter_push"), _bc.Code("armour_push"), _bc.Code("armour_female"), _bc.Code("armour_warp"), _bc.Code("rename_pak"),
                                _env_scoped("armour_winter_push", "armour_push", "armour_female", "armour_warp", "rename_pak")] + [_bc.File(s) for s in srcs],
               make, files=outs)
    for a in dc_data_patches.WINTER_UNDER_COPIES:
        assert "actor " + a in dc_data_patches.MP_LEVEL_ADDS, f"{a}: staged but not in MP_LEVEL_ADDS"
        p = "actor97/" + a + ".pak"
        for lst in (paths, order):
            if p not in lst:
                anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
                lst.insert(lst.index(anchor) + 1, p)
    print(f"model: winter pushed copies {', '.join(dc_data_patches.WINTER_UNDER_COPIES)}")


def stage_scar_gloves(paths, order):
    """actor97/<part>.pak of dc_data_patches.WINTER_GLOVE_ACTORS (T2R_MP_SNOW=1 + T2R_FACTION_LOOKS=1, opt-out T2R_WINTER_GLOVES=0): Owen's leather gloves as one
    base-male-skel part per hand and the Seraphite jacket kit copies with the hand skin under the glove cut (tools/models/scar_gloves.py, re/mp-snow.md 8.16).
    Verifies that every part WINTER_PART_SWAPS puts into a winter look is a geo part of a staged pak."""
    if not dc_data_patches.WINTER_GLOVES_ON:
        return
    sys.path.insert(0, os.path.join(TOOLS, "models"))
    import scar_gloves
    assert scar_gloves.actors() == dc_data_patches.WINTER_GLOVE_ACTORS and scar_gloves.GLOVES == dc_data_patches.WINTER_GLOVE_PARTS, "scar_gloves != dc_data_patches"
    outs = [os.path.join(STAGE, "actor97", a + ".pak") for a in dc_data_patches.WINTER_GLOVE_ACTORS]

    def make():
        rep = {}
        scar_gloves.build(os.path.join(STAGE, "actor97"), report=rep)
        if mp_snow.XPAK_ON:        # the jacket copies are built from the stock kits: give their garments the same material swap (+ own snow patch map) as the staged kits (rows are per material path, shared)
            import mp_snow_pak
            for c in "abc":
                kit = f"scar-male-{c}-jacket-kit"
                f = os.path.join(STAGE, "actor97", f"scar-glv1-{c}-jacket-kit.pak")
                data = open(f, "rb").read()
                for (k, g), (dpk_name, dn, _prm) in mp_snow.DONOR_OF.items():
                    if k == kit:
                        try:
                            data = mp_snow_pak.build(data, g, donor=dn, report={}, donor_pak=_game_pak(dpk_name), mask=True)
                        except mp_snow_pak.CliFailed:
                            raise
                        except (SystemExit, StopIteration, KeyError) as e:
                            rep.setdefault("snow_skipped", []).append((c, g, repr(e)[:80]))
                open(f, "wb").write(data)
        return rep
    _bc.cached("scar-gloves", [_bc.Code("scar_gloves"), _bc.Code("crew_retarget"), _bc.Code("rename_pak"), _bc.Code("mp_snow_pak"), _bc.Code("snow_clone"), _env_scoped("scar_gloves", "crew_retarget", "rename_pak", "mp_snow_pak", "snow_clone"), repr(sorted(mp_snow.DONOR_OF)),
                               _snow_inputs()], make, files=outs)
    scar_gloves.verify(os.path.join(STAGE, "actor97"))
    import armour_paks
    import armour_rattler
    staged = set()
    for a in dc_data_patches.WINTER_GLOVE_ACTORS:
        staged |= set(armour_rattler.geo_names(armour_paks._load(open(os.path.join(STAGE, "actor97", a + ".pak"), "rb").read())))
        assert "actor " + a in dc_data_patches.MP_LEVEL_ADDS, f"{a}: staged but not in MP_LEVEL_ADDS"
        p = "actor97/" + a + ".pak"
        for lst in (paths, order):
            if p not in lst:
                anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
                lst.insert(lst.index(anchor) + 1, p)
    need = {x for l, sw in dc_data_patches.WINTER_PART_SWAPS.items() if l not in dc_data_patches.WINTER_LOOKS or dc_data_patches.WINTER_LOOKS[l][0].startswith("scar-male-")
            for v in sw.values() for x in v}            # (the female looks' swaps: stage_scar_female_winter)
    assert need <= staged, f"winter glove parts not staged: {sorted(need - staged)}"
    print(f"model: Seraphite winter gloves {', '.join(dc_data_patches.WINTER_GLOVE_ACTORS)} ({sum(os.path.getsize(o) for o in outs)} bytes), "
          f"{len(dc_data_patches.WINTER_PART_SWAPS)} looks swap their loarm parts")


def stage_scar_female_winter(paths, order):
    """actor97/<part>.pak of dc_data_patches.WINTER_FSCAR_ACTORS (T2R_MP_SNOW=1 + T2R_FACTION_LOOKS=1 + T2R_FEMALE on, opt-out T2R_WINTER_FEMALE_SCAR=0): the female
    Seraphite winter looks' half mask (tools/models/scar_mask_female.py) and, with the gloves on, Owen's gloves on base-female-skel + the a-jacket copy with the hand
    skin cut (scar_gloves.py sex "f"); re/mp-snow.md 8.18. Verifies the female winter looks' new parts are geo parts of a staged pak."""
    if not dc_data_patches.WINTER_FSCAR_ON:
        return
    sys.path.insert(0, os.path.join(TOOLS, "models"))
    import scar_gloves
    import scar_mask_female
    assert (scar_mask_female.ACTOR, scar_mask_female.PART) == dc_data_patches.WINTER_FSCAR_MASK, "scar_mask_female != dc_data_patches"
    assert not dc_data_patches.WINTER_FGLOVES_ON or (scar_gloves.actors("f") == dc_data_patches.WINTER_FGLOVE_ACTORS and
                                                     scar_gloves.GLOVES_F == dc_data_patches.WINTER_FGLOVE_PARTS), "scar_gloves (female) != dc_data_patches"
    outs = [os.path.join(STAGE, "actor97", a + ".pak") for a in dc_data_patches.WINTER_FSCAR_ACTORS]

    def make():
        rep = {}
        scar_mask_female.build(os.path.join(STAGE, "actor97"), report=rep)
        if dc_data_patches.WINTER_FGLOVES_ON:
            scar_gloves.build(os.path.join(STAGE, "actor97"), report=rep, sex="f")
            if mp_snow.XPAK_ON:        # the jacket copy: the same material swap (+ own snow patch map) as the staged female kit, when mp_snow has one for it
                import mp_snow_pak
                for c in scar_gloves.KIND["f"]["jackets"]:
                    kit, f = scar_gloves.jacket_src(c, "f"), os.path.join(STAGE, "actor97", scar_gloves.jacket_copy(c, "f") + ".pak")
                    data = open(f, "rb").read()
                    for (k, g), (dpk_name, dn, _prm) in mp_snow.DONOR_OF.items():
                        if k == kit:
                            try:
                                data = mp_snow_pak.build(data, g, donor=dn, report={}, donor_pak=_game_pak(dpk_name), mask=True)
                            except mp_snow_pak.CliFailed:
                                raise
                            except (SystemExit, StopIteration, KeyError) as e:
                                rep.setdefault("snow_skipped", []).append((c, g, repr(e)[:80]))
                    open(f, "wb").write(data)
        return rep
    _bc.cached("scar-female-winter", [_bc.Code("scar_mask_female"), _bc.Code("scar_mask"), _bc.Code("scar_gloves"), _bc.Code("crew_retarget"), _bc.Code("rename_pak"),
                                      _bc.Code("mp_snow_pak"), _bc.Code("snow_clone"), _bc.Code("snow_mask"),
                                      _env_scoped("scar_mask_female", "scar_mask", "scar_gloves", "crew_retarget", "rename_pak", "mp_snow_pak", "snow_clone"),
                                      repr(sorted(mp_snow.DONOR_OF)), repr(dc_data_patches.WINTER_FSCAR_ACTORS), _snow_inputs()], make, files=outs)
    scar_mask_female.verify(os.path.join(STAGE, "actor97", scar_mask_female.ACTOR + ".pak"))
    if dc_data_patches.WINTER_FGLOVES_ON:
        scar_gloves.verify(os.path.join(STAGE, "actor97"), "f")
    import armour_paks
    import armour_rattler
    staged = set()
    for a in dc_data_patches.WINTER_FSCAR_ACTORS:
        staged |= set(armour_rattler.geo_names(armour_paks._load(open(os.path.join(STAGE, "actor97", a + ".pak"), "rb").read())))
        assert "actor " + a in dc_data_patches.MP_LEVEL_ADDS, f"{a}: staged but not in MP_LEVEL_ADDS"
        p = "actor97/" + a + ".pak"
        for lst in (paths, order):
            if p not in lst:
                anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
                lst.insert(lst.index(anchor) + 1, p)
    looks = [l for l, v in dc_data_patches.WINTER_LOOKS.items() if v[0].startswith("scar-female-")]
    need = {x for l in looks for x in dc_data_patches.WINTER_LOOKS[l][1]} | \
        {x for l in looks for v in dc_data_patches.WINTER_PART_SWAPS.get(l, {}).values() for x in v}
    assert looks and need <= staged, f"female winter parts not staged: {sorted(need - staged)}"
    print(f"model: female Seraphite winter looks {', '.join(looks)}: {', '.join(dc_data_patches.WINTER_FSCAR_ACTORS)} ({sum(os.path.getsize(o) for o in outs)} bytes)")


def stage_plant_anim(paths, order):
    """actor97/anim-ellie-boss.pak (dc_data_patches.PB_PLANT_ANIM_PAK): the placed pipe bomb's setting clip ellie-boss-crouch-plant-trap-mine
    (placedbomb2013.c PB_PLANT_CLIP). The game's copy is only in world-abby-fights-militia.psarc (not mounted in MP): read out of it (cached
    in re/gamebin/actor97), effect events stripped (its deploy sfx is played by the ext at the release), shipped unchanged otherwise."""
    name = dc_data_patches.PB_PLANT_ANIM_PAK
    uc4 = os.path.join(ROOT, "re", "uc4ps4", "tools")       # pk (the effect strip) lives there; stage_uc4_downed may not have run
    if uc4 not in sys.path:
        sys.path.insert(0, uc4)
    src = os.path.join(ROOT, "re", "gamebin", "actor97", name + ".pak")
    if not os.path.exists(src):
        import dsar_list
        game = os.environ.get("T2_GAME", "C:/Games/The Last of Us Part II Remastered")
        d, ent, bs, bsz = dsar_list.open_files(os.path.join(game, "build", "pc", "main", dc_data_patches.PB_PLANT_ANIM_ARCHIVE))
        os.makedirs(os.path.dirname(src), exist_ok=True)
        open(src, "wb").write(dsar_list.extract(d, ent, bs, bsz, "actor97/" + name + ".pak"))
    data = open(src, "rb").read()
    if b"ellie-boss-crouch-plant-trap-mine" not in data:
        raise SystemExit(f"{name}.pak has no ellie-boss-crouch-plant-trap-mine (placed bomb setting clip)")
    _stage_anim_pak(paths, order, name, data, "placed bomb setting clip")


def stage_parry_anim(paths, order):
    """actor97/anim-melee-abby-ellie-fight.pak (dc_data_patches.PARRY_STAGED_PAK): the Ellie-vs-Abby fight's abby-vs-ellie-* clips (Abby's punches, Ellie's knife /
    gun parries) for the melee duel's parry pairs (melee_duel_parry.c). Only in world-abby-fights-militia.psarc (not mounted in MP): read out of it (cached in
    re/gamebin/actor97), effect events stripped like the plant clip pak, shipped otherwise unchanged. The other four parry paks are sp-common ones (loaded as is)."""
    if not dc_data_patches.PARRY_PAKS_ON:
        return
    name = dc_data_patches.PARRY_STAGED_PAK
    uc4 = os.path.join(ROOT, "re", "uc4ps4", "tools")
    if uc4 not in sys.path:
        sys.path.insert(0, uc4)
    src = os.path.join(ROOT, "re", "gamebin", "actor97", name + ".pak")
    if not os.path.exists(src):
        import dsar_list
        game = os.environ.get("T2_GAME", "C:/Games/The Last of Us Part II Remastered")
        d, ent, bs, bsz = dsar_list.open_files(os.path.join(game, "build", "pc", "main", dc_data_patches.PARRY_STAGED_ARCHIVE))
        os.makedirs(os.path.dirname(src), exist_ok=True)
        open(src, "wb").write(dsar_list.extract(d, ent, bs, bsz, "actor97/" + name + ".pak"))
    data = open(src, "rb").read()
    if b"abby-vs-ellie-knife-parry-right--abby" not in data:
        raise SystemExit(f"{name}.pak has no abby-vs-ellie-knife-parry-right--abby (duel parry clip)")
    _stage_anim_pak(paths, order, name, data, "duel parry (Ellie parries Abby)")
    if dc_data_patches.PARRY_REBIND_ON:
        # the Santa Barbara fight's Abby halves (abby-prisoner-skel, not loadable in MP - dc_data_patches.PARRY_REBIND_*) re-bound to the base-female rig id
        import struct
        models = os.path.join(ROOT, "tools", "models")
        if models not in sys.path:
            sys.path.insert(0, models)
        import ndpak
        name = dc_data_patches.PARRY_REBIND_PAK
        _game_pak(name)
        pk = ndpak.Pak(os.path.join(ROOT, "re", "gamebin", "actor97", name + ".pak"))
        data = bytearray(pk.d)
        n = 0
        for nm, typ, pi, io in pk.items:
            if typ == "ANIM" and pk.u32(io + 0x50) == dc_data_patches.PARRY_REBIND_FROM:
                if pk.u32(io + 0x60) != dc_data_patches.PARRY_REBIND_LAYOUT:
                    raise SystemExit(f"{name}: {nm} rig layout word {pk.u32(io + 0x60):#x} != {dc_data_patches.PARRY_REBIND_LAYOUT:#x} (not base-female compatible)")
                struct.pack_into("<I", data, io + 0x50, dc_data_patches.PARRY_REBIND_TO)
                n += 1
        if n != 109:
            raise SystemExit(f"{name}: {n} abby-prisoner-skel clips (109 expected) - the pak changed")
        # (not through _stage_anim_pak: that strips every clip's effect events - this pak's own foley / hit sounds of the parry pairs must stay)
        open(os.path.join(STAGE, "actor97", name + ".pak"), "wb").write(bytes(data))
        p = "actor97/" + name + ".pak"
        for lst in (paths, order):
            if p not in lst:
                lst.insert(lst.index("actor97/rifle-autorifle4.pak") + 1, p)
        print(f"anim: boss fight pak {name} ({len(data)} bytes) in the MP level - {n} abby-prisoner-skel clips re-bound to the base-female rig, events kept")


def stage_glint_part(paths, order):
    """pak68/part-rifle-tommy.pak (dc_data_patches.GLINT_PART): the particle module with TLOU2's Tommy-sniper lens glint (sniper-scope-flare-1, scopeglint.c).
    Only in world-ellie-flashback-patrol.psarc (not mounted in MP): read out of it (cached in re/gamebin/pak68), shipped unchanged; the MP level
    lists it (MP_LEVEL_ADDS: package / package -dict / particle-module, the game's own block of that level)."""
    if not dc_data_patches.GLINT_ON:
        return
    name = dc_data_patches.GLINT_PART
    src = os.path.join(ROOT, "re", "gamebin", "pak68", name + ".pak")
    if not os.path.exists(src):
        import dsar_list
        game = os.environ.get("T2_GAME", "C:/Games/The Last of Us Part II Remastered")
        d, ent, bs, bsz = dsar_list.open_files(os.path.join(game, "build", "pc", "main", dc_data_patches.GLINT_ARCHIVE))
        os.makedirs(os.path.dirname(src), exist_ok=True)
        open(src, "wb").write(dsar_list.extract(d, ent, bs, bsz, "pak68/" + name + ".pak"))
    data = open(src, "rb").read()
    if b"sle-offscreen-fake-lensflare" not in data:
        raise SystemExit(f"{name}.pak has no sle-offscreen-fake-lensflare particle (scope glint module)")
    os.makedirs(os.path.join(STAGE, "pak68"), exist_ok=True)
    open(os.path.join(STAGE, "pak68", name + ".pak"), "wb").write(data)
    p = "pak68/" + name + ".pak"
    for lst in (paths, order):
        if p not in lst:
            last = max(i for i, x in enumerate(lst) if x.startswith("pak68/"))
            lst.insert(last + 1, p)
    print(f"glint: pak68/{name}.pak ({len(data)} bytes) shipped, listed in the MP level")


def stage_exec_recoil(paths, order):
    """actor97/anim-ellie-horse.pak (the horse-riding animation pak, the game's) with the five SCALED gun-fire recoil clips pooled into additive horse-riding carriers
    (tools/features/exec_recoil_clips.py, exec_recoil_gestures.py): the gesture defs of the exec_recoil overlay play them at each execution shot.
    Decoded back and compared; every other clip is checked byte-identical. T2R_EXEC_RECOIL=0 = not shipped."""
    import exec_recoil_clips
    dst = os.path.join(STAGE, "actor97", "anim-ellie-horse.pak")
    old = open(dst, "rb").read() if os.path.exists(dst) else exec_recoil_clips.game_pak("anim-ellie-horse.pak")
    new, clips = exec_recoil_clips.build(old)
    err = exec_recoil_clips.check(new, old, clips)
    if err > 1e-6:
        raise SystemExit(f"exec recoil clips do not decode back (max error {err:.2e})")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, "wb").write(new)
    p = "actor97/anim-ellie-horse.pak"
    for lst in (paths, order):
        if p not in lst:
            anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
            lst.insert(lst.index(anchor) + 1, p)


def stage_gx_weapon(paths, order):
    """actor97/pistol-base.pak (the one the Deagle slide build staged, else the game's) with the 2013 revolver cylinder spin baked into its unused
    'scale-test' clip (tools/models/gx_weapon_clips.py; the ext plays it on the weapon at the 2013 time of execution-revolver-left)."""
    sys.path.insert(0, os.path.join(TOOLS, "models"))
    import deagle_slide
    import gx_weapon_clips
    dst = os.path.join(STAGE, "actor97", "pistol-base.pak")
    data = open(dst, "rb").read() if os.path.exists(dst) else deagle_slide.game_pak("pistol-base.pak")
    new, clips = _bc.cached("gx-weapon-clips", [_bc.Code("gx_weapon_clips"), _env_scoped("gx_weapon_clips"), data],
                            lambda: gx_weapon_clips.build(data))
    if not os.path.exists(dst):       # (against the game's pak; a Deagle-slide staged pak differs in other clips by design)
        deagle_slide.sanity("pistol-base.pak", new, gx_weapon_clips.report())
    err, aerr = _bc.cached("gx-weapon-check", [_bc.Code("gx_weapon_clips"), _env_scoped("gx_weapon_clips"), new],
                           lambda: gx_weapon_clips.check(new))
    if err > 1e-3 or aerr > 0.05:
        raise SystemExit(f"gx weapon clip does not decode back (row error {err:.2e}, cylinder angle error {aerr:.3f} deg)")
    open(dst, "wb").write(new)
    p = "actor97/pistol-base.pak"
    for lst in (paths, order):
        if p not in lst:
            anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
            lst.insert(lst.index(anchor) + 1, p)
    print(f"anim: pistol-base.pak - 2013 revolver crane open + cylinder spin in clips {[c['carrier'] for c in gx_weapon_clips.CLIPS.values()]} ({sum(len(c) for c in clips)} bytes)")


def _gx_cached(mod):
    """build_gunexec_pak.build (plan + bake the 2013 clips into a game anim pak, ~4 s a pak) as a cached step: a pure function of the
    game's pak, the baked clips (re/factions2013/t1anim, in _env()) and its code; build_all / _gunexec_third call it through the module"""
    if getattr(mod, "_t2r_cached", False):
        return
    orig = mod.build

    def cached_build(data, pak=None):
        pak = pak or mod.PAK
        mod.PAK = pak
        return _bc.cached("gunexec-build-" + pak, [_bc.Code("build_gunexec_pak"), _env_scoped("build_gunexec_pak"), data, pak], lambda: orig(data, pak))
    mod.build = cached_build
    mod._t2r_cached = True


def _gunexec_third(name):
    """(pak bytes with the 2013 gun execution clips, res) of the gun execution set that lives in the game pak `name` (None = no such set / gun executions off):
    the Frontier lever step starts from it so the two share anim-npc-m-lg (their carriers are disjoint)"""
    if not GUNEXEC or name not in GUNEXEC_PAKS:
        return None
    if name not in _GX_BUILT:
        t1 = os.path.join(ROOT, "re", "factions2013", "t1anim")
        if t1 not in sys.path:
            sys.path.insert(0, t1)
        import build_gunexec_pak
        _gx_cached(build_gunexec_pak)
        build_gunexec_pak.PAK = name
        _GX_BUILT[name] = build_gunexec_pak.build(_game_pak(name), name)
    return _GX_BUILT[name]


def stage_gunexec(paths, order):
    """actor97/anim-npc-alert-caps.pak with the TLOU1 gun execution clips baked in (re/factions2013/t1anim,
    gun-executions-plan.md section 5; tools/ext/gunexec_carriers.h maps the ext's t1gx-* names to the carriers)."""
    t1 = os.path.join(ROOT, "re", "factions2013", "t1anim")
    sys.path.insert(0, t1)
    import build_gunexec_pak
    _gx_cached(build_gunexec_pak)
    skip = [n for n, _ in build_gunexec_pak.PAK_SETS if n not in GUNEXEC_PAKS]
    assert [n for n, _ in build_gunexec_pak.PAK_SETS if n not in skip] == GUNEXEC_PAKS, "build_gunexec_pak.PAK_SETS and GUNEXEC_PAKS differ"
    built = build_gunexec_pak.build_all(_game_pak, skip, _GX_BUILT)
    res = build_gunexec_pak.all_res(built)
    for name, data, r in built:
        if not _verify_cached("gunexec-" + name, ["build_gunexec_pak"], [data, r], lambda: build_gunexec_pak.verify(data, r),
                              stats=[os.path.join(ROOT, "re", "uc4ps4"), t1]):
            raise SystemExit(f"gun execution carrier pak {name} does not decode back to the baked clips")
    for path, text in ((build_gunexec_pak.HEADER, build_gunexec_pak.header(res)),
                       (build_gunexec_pak.AHEADER, build_gunexec_pak.aheader(res)),
                       (build_gunexec_pak.MHEADER, build_gunexec_pak.mheader(res)),
                       (build_gunexec_pak.VHEADER, build_gunexec_pak.vheader(res)),
                       (build_gunexec_pak.DHEADER, build_gunexec_pak.dheader(res)),
                       (build_gunexec_pak.RHEADER, build_gunexec_pak.rheader(res)),
                       (build_gunexec_pak.SHEADER, build_gunexec_pak.sheader(res)),
                       (build_gunexec_pak.DHEADER13, build_gunexec_pak.nheader(res)),
                       (build_gunexec_pak.UHEADER, build_gunexec_pak.durheader(res))):
        with open(path, encoding="utf-8") as f:
            cur = f.read()
        if cur != text:     # (twice a deploy shipped old data because of this) - rewrite it; deploy_test.sh rebuilds the ext after
            with open(path, "w", encoding="utf-8", newline=chr(10)) as f:
                f.write(text)
            print(f"tools/ext/{os.path.basename(path)} was out of date - rewritten (the ext is rebuilt after the data build)")
    for name, data, r in built:
        if name in _GX_FRONTIER_STAGED:       # the Frontier lever already staged this pak starting from our output: the clips must still decode back from the FINAL file
            final = open(os.path.join(STAGE, "actor97", name + ".pak"), "rb").read()
            if not _verify_cached("gunexec-final-" + name, ["build_gunexec_pak"], [final, r], lambda: build_gunexec_pak.verify(final, r),
                                  stats=[os.path.join(ROOT, "re", "uc4ps4"), t1]):
                raise SystemExit(f"gun execution clips do not decode back from the staged {name} (Frontier chain)")
            print(f"anim: gun execution clips ({len(r)}) ride in the Frontier-staged {name}")
            continue
        _stage_anim_pak(paths, order, name, data, f"gun execution ({len(r)} clips)")


def stage_holster_draw(paths, order):
    """sidearms draw from the right hip / thigh holster, not from behind the belt (dc_data_patches.patch_holster_draw: Abby's second sidearm
    family = her holster draw, in every overlay set of player-overlays.bin; same file size). The staged file is the contributed copy when there is one
    (the overlay loop above put it in the stage), else the game's. T2R_HOLSTER_DRAW=0 = off."""
    if os.environ.get("T2R_HOLSTER_DRAW", "1") == "0":
        print("holster draw: off (T2R_HOLSTER_DRAW=0) - sidearms keep Abby's back-of-belt draw")
        return
    sys.path.insert(0, os.path.join(TOOLS, "part1"))
    import p1_carrier
    rel = "bin/dc1/player-overlays.bin"
    dst = os.path.join(STAGE, *rel.split("/"))
    if not os.path.exists(dst):
        shutil.copyfile(os.path.join(ROOT, "re", "gamebin", "dc1", "player-overlays.bin"), dst)
        for lst in (paths, order):
            if rel not in lst:
                last = max(i for i, x in enumerate(lst) if x.startswith("bin/dc1/"))
                lst.insert(last + 1, rel)
    clips = set(p1_carrier.anims(p1_carrier.load(p1_carrier.game_pak("anim-player-gun-abby"))))
    dc_data_patches.patch_holster_draw(dst, clips)


def stage_db_reload(paths, order, warp=True):
    """Double Barrel reload with eased timing (tools/part1/db_reload.py: same 66 frames, source time re-mapped per segment, committed clips in
    tools/features/data/db-reload).  The baked part / add / gun clips ride on clip slots of three SP-only Abby workbench paks (p1_carrier.build,
    like the Frontier's lever clips).  They replace the clips MP really plays, shotgun-baserri-reload-<stance>-<layer> (the -stock- variants
    need the SP stock upgrade's overlay layer, which MP never sets: ovdbg "no entry"), through t2r-ext's overlay find-entry hook:
    tools/ext/db_reload_built.h (written here, compiled into the next t2r-ext) maps each played name to its carrier and is answered when the
    engine's own lookup finds no entry (so the aim / supine / prone renames still run first).  T2R_DB_RELOAD_WARP=0: the game's own clips,
    an empty header (the overlay module is still staged for its grip fix, below).  Returns the MP level adds (the carrier paks)."""
    sys.path.insert(0, os.path.join(TOOLS, "part1"))
    import db_reload
    import p1_carrier
    import db_hold
    jobs = db_reload.load() if warp else {"body": [], "gun": []}
    # DB aim hold (tools/part1/db_hold.py: the stand / crouch aim adds re-solved on base-male-skel so the stock seats as in SP; the ext also
    # puts the DB on Abby's rifle aim base); T2R_DB_HOLD=0 leaves the game's adds
    played = {}
    if os.environ.get("T2R_DB_HOLD", "1") != "0":
        hj, played = db_hold.load()
        jobs["body"] = jobs["body"] + hj
    # Semi-Auto Rifle aim hold (it did not dig into the shoulder): its rifle-tommy-abby-player set plays Abby's Vepr aim adds; the same
    # re-solve for the M14 model (db_hold.py m14).  The ext answers rifle-vepr-grip-{stand,crouch}-aim-add with them ONLY while the M14's set is on
    # (the Vepr plays the same names).  T2R_M14_HOLD=0 leaves the game's adds.
    gated = []
    if os.environ.get("T2R_M14_HOLD", "1") != "0":
        hj, p2 = db_hold.load("m14")
        jobs["body"] = jobs["body"] + hj
        played = {**played, **p2}
        gated += list(p2)
    # Semi-Auto Rifle right hand (the primary hand acted as if holding a PISTOL GRIP): tools/part1/m14_hand.py re-bakes every
    # absolute rifle-vepr-grip-* clip the M14's set plays with the hunting rifle's stock grip (r_hand_prop + fingers, right arm re-solved so the
    # gun keeps its pose); the aim adds above carry the same hand.  Gated like the adds.  T2R_M14_HAND=0 leaves the Vepr hand.
    if os.environ.get("T2R_M14_HOLD", "1") != "0" and os.environ.get("T2R_M14_HAND", "1") != "0":
        import m14_hand
        hj, p3 = m14_hand.load()
        jobs["body"] = jobs["body"] + hj
        played = {**played, **p3}
        gated += list(p3)
    names = {}
    adds = []
    for pak, bsig, use_gun, js in (db_reload.carriers(jobs) if jobs["body"] or jobs["gun"] else []):
        # (body sig 1: the body clips of a gun-only host pak stay untouched, sig 1 matches no rig)
        data, nm = p1_carrier.build(js, print, pak=pak, body_sig=bsig, gun_sig=db_reload.GUN_SIG, use_gun=use_gun)
        p1_carrier.check(data, {nm[j[0]]: j[1] for j in js})
        os.makedirs(os.path.join(STAGE, "actor97"), exist_ok=True)
        open(os.path.join(STAGE, "actor97", pak + ".pak"), "wb").write(data)
        cp = "actor97/" + pak + ".pak"
        for lst in (paths, order):
            if cp not in lst:
                anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
                lst.insert(lst.index(anchor) + 1, cp)
        adds.append("actor " + pak)
        names.update(nm)
    db_reload.write_ext_header(names, played, gated)
    rel = "player-specific-weapon-overlays/abby-guns/shotgun-dblbarrel-over-under.bin"
    d = bytearray(open(os.path.join(ROOT, "re", "gamebin", "dc1", *rel.split("/")), "rb").read())
    # grip fix (held wrong; use shotgun-dbl-barrel-grip-stand-aim-hands in the file): see dc_data_patches.patch_db_grips
    for line in dc_data_patches.patch_db_grips(d, os.path.join(TOOLS, "win", "ext", "dcmain", "build", "sidbase.bin")):
        print("DB grip: " + line)
    # the game's module is registered (startup.bin / modules.bin name + sid, size 46504 = this file) as abby-guns/shotgun-baserri-over-under; the
    # psarc lists the same bytes as ...-dblbarrel-over-under.bin. Which of the two names the loader asks the file system for is not known
    # offline ("same animation" - the dblbarrel-only copy changed nothing), so BOTH paths carry the patched module (same size: modules.bin
    # needs no fix; the one the loader does not ask for is never read).
    for r in (rel, rel.replace("shotgun-dblbarrel-over-under", "shotgun-baserri-over-under")):
        dst = os.path.join(STAGE, "bin", "dc1", *r.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, "wb").write(d)
        f = "bin/dc1/" + r
        for lst in (paths, order):
            if f not in lst:
                lst.insert(lst.index("bin/dc1/weapon-gameplay.bin") + 1, f)
    print(f"DB reload: {len(names)} re-timed clips on {', '.join(a[6:] for a in adds)} (t2r-ext answers their played names: tools/ext/"
          f"db_reload_built.h), overlay module {rel} (+ its baserri-over-under name) staged for the grip fix")
    return adds


def stage_frontier(paths, order, wa, wg, mdata):
    """2013 Frontier Rifle (re/factions2013/frontier-rifle.md): rifle-frontier = the hunting rifle's clone on the Marlin lever model,
    the NPC lever cycle on its gun layer (player overlay module = the game's loaded leftover player-specific-weapon-overlays/
    rifle-lever-action with the hunting rifle's full set + the lever renames), the lever bank with our own mixed shot."""
    import weapon_clone
    import frontier_sound
    import sndbank
    sys.path.insert(0, os.path.join(TOOLS, "part1"))
    import p1_overlay
    dc1 = os.path.join(STAGE, "bin", "dc1")
    D = dc_data_patches
    weapon_clone.clone_weapon(dc1, D.FRONTIER_SRC, D.FRONTIER, D.FRONTIER_SFX, D.FRONTIER_AUDIO, None, D.FRONTIER_ACTOR, None,
                              D.FRONTIER_MUZZLE_DZ, upgrades=False)
    D.patch_m14_gameplay(wg, D.FRONTIER, D.FRONTIER_STATS)
    D.patch_galil_interval(wg)                                       # 2013 0.10 s (the Galil was weaker than the 9mm)
    D.patch_tac_reticle(wg, D.FRONTIER, D.FRONTIER_RETICLES)        # 2013: a thin ring with a centre dot (the circle reticle)
    weapon_clone.copy_art_fields(wa, D.FRONTIER, "rifle-lever-action", (0x180, 0x188))     # player overlay key (+0x180 / +0x188)
    D.patch_frontier_art(wa)
    # player overlay module: the hunting rifle's set (grips, aim, fire, every reload clip) + the NPC lever cycle on the gun layer
    host = os.path.join(dc1, "player-specific-weapon-overlays", *D.FRONTIER_OVERLAY_HOST[0].split("/"))
    if not os.path.exists(host):
        host = os.path.join(ROOT, "re", "gamebin", "dc1", "player-specific-weapon-overlays", *D.FRONTIER_OVERLAY_HOST[0].split("/"))
    rel = f"player-specific-weapon-overlays/{D.FRONTIER_OVERLAY_KEY}.bin"
    dst = os.path.join(dc1, *rel.split("/"))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    renames = {c: D.FRONTIER_RECHAMBER_GUN for c in D.FRONTIER_GUN_CLIPS}
    # the NPC lever's arms / spine layers baked onto the player rig (tools/part1/frontier_anims.py rechamber, frontier_reload.py reload;
    # committed clips, no emulator here), carried by clips of two SP workbench paks no MP match plays
    import frontier_anims
    import frontier_reload
    import p1_carrier
    clips, ev = frontier_anims.load()
    rclips, rev = frontier_reload.load()
    isgun = lambda k: k == "gun" or k.startswith("rgun-")
    # (the rechamber GUN layer is the NPC's own clip, loaded from anim-npc-m-lg: the mechanism of the first Frontier build, the one whose lever
    # joint moved in game; the baked gun.clip is not shipped)
    jobs = [(f"frontier|{k}", c, "gun" if isgun(k) else ("add" if k == "spine" else "part"), None if (k == "spine" or isgun(k)) else ev)
            for k, c in clips.items() if k != "gun" or D.FRONTIER_FIREGUN]
    # reload arms: the hunting rifle's own clips with the RIGHT arm on the lever (frontier_reload.bake_right), the bolt clips' own anim events
    pk_player = p1_carrier.game_pak(frontier_reload.fa.PLAYER_PAK)
    for key, c in frontier_reload.load_right().items():
        st, seg = key.rsplit("-", 1)
        evs = p1_carrier.ref_events(pk_player, f"ellie-sniper-bolt-reload-{st}-{seg}-arms")[1]
        jobs.append((f"rarm|{key}", c, "part", evs))
    for k, c in (rclips.items() if D.FRONTIER_LEVER_RELOAD else ()):      # (off: the Frontier reloads with the hunting rifle's own clips)
        kind = k.rsplit("-", 1)[1]
        jobs.append((f"reload|{k}", c, "gun" if kind == "gun" else ("add" if kind == "spine" else "part"),
                     rev[k[:-5]] if kind == "arms" else None))
    # the carriers: clip slots of three SP workbench paks no MP match plays (body slots / rifle-base gun slots per pak, filled in order)
    caps = []
    for pak in D.FRONTIER_CARRIER_PAKS:
        a_ = p1_carrier.anims(p1_carrier.load(p1_carrier.game_pak(pak)))
        caps.append([sum(1 for v in a_.values() if v[3] == p1_carrier.ELLIE_SIG), sum(1 for v in a_.values() if v[3] == frontier_reload.GUN_SIG)])
    per_pak = [[] for _ in caps]
    for job in jobs:
        gi = 1 if job[2] == "gun" else 0
        for i, cap in enumerate(caps):
            if cap[gi] > 0:
                cap[gi] -= 1
                per_pak[i].append(job)
                break
        else:
            raise SystemExit(f"Frontier carriers: no free {'gun' if gi else 'body'} slot for {job[0]}")
    names = {}
    os.makedirs(os.path.join(STAGE, "actor97"), exist_ok=True)
    for pi, pak in enumerate(D.FRONTIER_CARRIER_PAKS):
        pjobs = per_pak[pi]
        if not pjobs:
            continue
        while True:     # slots are counted above, bytes are not: a full pool spills its biggest clip to a later pak with a free slot
            try:
                cdata, cn = p1_carrier.build(pjobs, print, pak=pak, use_gun=any(j[2] == "gun" for j in pjobs), gun_sig=frontier_reload.GUN_SIG)
                break
            except SystemExit as e:
                if "carrier pool full" not in str(e):
                    raise
                big = max(pjobs, key=lambda j: len(j[1]) if isinstance(j[1], (bytes, bytearray)) else len(getattr(j[1], "data", b"") or b""))
                gi = 1 if big[2] == "gun" else 0
                to = next((k for k in range(pi + 1, len(caps)) if caps[k][gi] > 0), None)
                if to is None:
                    raise SystemExit(f"Frontier carriers: {pak} pool full and no later pak has a free {'gun' if gi else 'body'} slot for {big[0]}")
                caps[to][gi] -= 1
                pjobs.remove(big)
                per_pak[to].append(big)
                print(f"anim: Frontier carrier {pak} pool full - {big[0]} moved to {D.FRONTIER_CARRIER_PAKS[to]}")
        p1_carrier.check(cdata, {cn[k]: c for k, c, _, _ in pjobs})
        names.update(cn)
        open(os.path.join(STAGE, "actor97", pak + ".pak"), "wb").write(cdata)
        cp = "actor97/" + pak + ".pak"
        for lst in (paths, order):
            if cp not in lst:
                anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
                lst.insert(lst.index(anchor) + 1, cp)
    cnames = names
    if D.FRONTIER_FIREGUN:       # (off: 130880c, the first build whose lever moved, left the art def's fire-gun +0x10 on the hunting rifle's)
        D.patch_frontier_fire_gun(wa, cnames["frontier|gun"])         # the direct path: the lever cycle as the gun layer of every shot
    renames = {c: D.FRONTIER_RECHAMBER_GUN for c in D.FRONTIER_GUN_CLIPS}
    for v in D.FRONTIER_VARIANTS:
        renames[f"rifle-remington-bolt-{v}rechamber-arms"] = cnames["frontier|arms-" + ((v or "aim-").rstrip("-"))]
        renames[f"rifle-remington-bolt-{v}rechamber-spine"] = cnames["frontier|spine"]
    for st in D.FRONTIER_RELOAD_STANCES:
        for seg in ("start", "mid", "end"):
            renames[f"ellie-sniper-bolt-reload-{st}-{seg}-arms"] = cnames[f"rarm|{st}-{seg}"]
    for st in D.FRONTIER_RELOAD_STANCES:                    # reload: the hunting rifle's clips, the GUN layer = the lever down (start) / open (mid) / up (end)
        for seg in ("start", "mid", "end"):
            renames[f"ellie-sniper-bolt-reload-{st}-{seg}-gun"] = cnames["frontier|rgun-" + frontier_anims.RELOAD_ODD_STANCE.get((st, seg), seg)]
    for st in (frontier_reload.STANCES if D.FRONTIER_LEVER_RELOAD else ()):   # the one-round-at-a-time reload: start / mid x N rounds / end
        grp = "crouch" if st.startswith("crouch") else "stand"
        for seg in frontier_reload.SEGS:
            for kind, key in (("arms", f"{st}-{seg}-arms"), ("spine", f"{grp}-{seg}-spine"), ("gun", f"{grp}-{seg}-gun")):
                renames[f"ellie-sniper-bolt-reload-{st}-{seg}-{kind}"] = cnames["reload|" + key]
    if os.environ.get("T2R_FRONTIER_WEDGE", "1") == "1":
        # The supplied lever (lever-action-rifle.psarc, which played the lever in game but edited the hunting rifle's own set + the global
        # player-overlays, so the bolt-action got it too): the lever cycle rides the FIRE clips (fire-start -> the NPC lever --add, fire-gun ->
        # its --gun) and every bolt rechamber layer is a null additive. Applied to the Frontier's own module only: the hunting rifle is untouched.
        # Slowed to 2013's rechamber speed, sounds with it: the NPC clips are 0.83 s with the lever in native frames
        # 7-16; their retimed copies (frontier_anims.bake_wedge: 25 frames @ 24/1.1 fps on WEDGE_KNOTS, lever down 0.40 .. 0.95 s, back by
        # 1.1 s) are carried by clip slots of the NPC pak itself (anim-npc-m-lg is in the MP level: dc_data_patches MP_LEVEL_ADDS; its 934
        # NPC-rig / 24 rifle-base clips are not played in MP).  The --add carrier holds the NPC clip's three lever weapon-effect sounds at
        # the retimed frames; the hunting rifle's bolt sounds sat on the rechamber arms / gun clips, which are null now.
        # the lever is on the RECHAMBER layers (frontier_anims.split_wedge): the fire clips keep only the shot's first 0.18 s, the rechamber state
        # plays the rest at def +0x2c (the ext's per-level lever rate) and ends on weapon-rechamber at the lever-shut frame - upgrades speed the lever up.
        # the FIRE clips are the hunting rifle's own again (dry fire moved the gun a lot - the engine derives the dry-fire clips from the
        # played fire clip's name via player-overlays.bin, a renamed fire-start played the lever's reach on every empty click); only the rechamber part is carried
        wclips, wev_fire, wev_rech = frontier_anims.split_wedge()
        wjobs = [("wedge|add-rech", wclips["add-rech"], "add", wev_rech), ("wedge|gun-rech", wclips["gun-rech"], "gun", None)]
        # the per-level lever speed is a per-body choice of CLIP (the def's +0x2c never moved the lever clips): carriers for level 1 / 2 (same keys
        # and events, fps scaled) - t2r-ext answers our own body's lookup of the level-0 carrier with the level's (frontier_lever_built.h)
        for lv_ in (1, 2):
            wjobs += [(f"wedge|add-rech{lv_}", wclips[f"add-rech{lv_}"], "add", wev_rech), (f"wedge|gun-rech{lv_}", wclips[f"gun-rech{lv_}"], "gun", None)]
        gx3 = _gunexec_third(D.FRONTIER_NPC_ANIM_PAK)
        wdata, wn = p1_carrier.build(wjobs, print, pak=D.FRONTIER_NPC_ANIM_PAK, use_gun=True, gun_sig=frontier_reload.GUN_SIG,
                                     body_sig=0xb813aaa4,
                                     keep_raw=True, src_bytes=gx3[0] if gx3 else None, exclude={r[0] for r in gx3[1]} if gx3 else ())
        if gx3:
            _GX_FRONTIER_STAGED.append(D.FRONTIER_NPC_ANIM_PAK)
        p1_carrier.check(wdata, {wn[k]: c for k, c, _, _ in wjobs})
        open(os.path.join(STAGE, "actor97", D.FRONTIER_NPC_ANIM_PAK + ".pak"), "wb").write(wdata)
        cp = "actor97/" + D.FRONTIER_NPC_ANIM_PAK + ".pak"
        for lst in (paths, order):
            if cp not in lst:
                anchor = "actor97/gui2-widgets-pc.pak" if "actor97/gui2-widgets-pc.pak" in lst else "actor97/gui2-widgets.pak"
                lst.insert(lst.index(anchor) + 1, cp)
        import struct as _st
        for k_, c_ in sorted(wclips.items()):          # (split_wedge: add-fire / gun-fire / add-rech / gun-rech)
            nfr_, fps_ = _st.unpack_from("<H", c_, 0xe)[0], _st.unpack_from("<f", c_, 0x10)[0] if False else frontier_anims.WEDGE_FPS
            print(f"frontier lever: carried {k_} clip: {nfr_} frames @{fps_:.3f} = {(nfr_ - 1) / fps_:.3f} s")
        print("frontier lever: rechamber events at frames " + ", ".join(f"{f:g} ({f / frontier_anims.WEDGE_FPS:.3f} s)" for _, f, _ in wev_rech))
        for v in D.FRONTIER_VARIANTS:
            for kind in ("arms", "spine", "gun", "sub"):
                renames[f"rifle-remington-bolt-{v}rechamber-{kind}"] = "bindpose-null-add"
            # the lever: arms (the NPC --add with the lever sounds + weapon-rechamber / abort-out) and gun layers of the rechamber state
            renames[f"rifle-remington-bolt-{v}rechamber-arms"] = wn["wedge|add-rech"]
            renames[f"rifle-remington-bolt-{v}rechamber-gun"] = wn["wedge|gun-rech"]
        print(f"frontier lever: every rechamber-arms -> {wn['wedge|add-rech']}, rechamber-gun -> {wn['wedge|gun-rech']}")
        frontier_anims.write_ext_header(wn)
        for kind in ("arms", "spine", "gun"):
            renames[f"sniper-bolt-rechamber-mirror--{kind}"] = "bindpose-null-add"
    n, ch, add = p1_overlay.rewrite(os.path.join(ROOT, "re", "gamebin", "dc1", *rel.split("/")), host, D.FRONTIER_OVERLAY_HOST[1],
                                    renames, dst)
    # 130880c (the first build whose lever cycled in game) kept the leftover lever module's OWN compiled map (94 keys, 12 infos); every later build
    # replaced it with the hunting rifle's (1145 keys, 166 infos: rebuild_map, cd6e926 / 21594c7) and the lever never moved again.  Default = 130880c's.
    if D.FRONTIER_HUNT_MAP:
        nk, na = p1_overlay.rebuild_map(dst, host, "*weapon-specific-overlay-rifle-remington-bolt*", "*weapon-specific-overlay-rifle-lever-action*", {})
    else:
        nk = "the lever module's own 94"
    # (the map is the hunting rifle's motion-matching -> grip table, every info a blend mask: our clip renames live in the SET only.
    # renames added to the map as arms-blend infos stopped the lever joint moving and made the left hand wander)
    print(f"overlay {rel}: compiled map: {nk} keys")
    print(f"overlay {rel}: {D.FRONTIER_OVERLAY_HOST[1]} ({n} entries: {ch} retargeted, {add} added; tree entries with module "
          f"pointers: {len(p1_overlay.rewrite.reused)} use the target's own lambdas, {len(p1_overlay.rewrite.dropped)} left out)")
    if os.environ.get("T2R_FRONTIER_WEDGE", "1") == "1":       # read the staged module back: the lever renames resolve to the carried clips
        from dcfile import DcFile, fnv as _fnv
        rd = DcFile(open(dst, "rb").read())
        got = {}
        for s_ in p1_overlay.find_sets(rd):
            for frm, to, _raw in p1_overlay.entries(rd, s_):
                got[frm] = to
        want = {k: v for k, v in renames.items() if k.startswith("rifle-remington-bolt-") and ("fire-start" in k or "fire-gun" in k or k.endswith("rechamber-arms") or k.endswith("rechamber-gun"))}
        for k, v in sorted(want.items()):
            ok = got.get(_fnv(k)) == _fnv(v)
            print(f"overlay readback: {k} -> {v}: {'OK' if ok else 'MISSING'}")
            if not ok:
                raise SystemExit(f"Frontier overlay: {k} does not resolve to {v}")
        nulls = sum(1 for k, v in renames.items() if v == "bindpose-null-add" and got.get(_fnv(k)) == _fnv(v))
        print(f"overlay readback: {nulls} rechamber layers -> bindpose-null-add; the hunting rifle's module {D.FRONTIER_OVERLAY_HOST[0]} is only read, never written")
    dc_modules.set_size(mdata, rel[:-4], os.path.getsize(dst))
    f = "bin/dc1/" + rel
    for lst in (paths, order):
        if f not in lst:
            lst.insert(lst.index("bin/dc1/weapon-gameplay.bin") + 1, f)
    # our own shot: the lever bank (sp-common soundbank4, no MP gun uses it) with the Frontier thumps + the hunting rifle's crack
    wav_dir = frontier_sound.build_wavs()
    lever = open(os.path.join(ROOT, "re", "sound", D.FRONTIER_BANK + ".bnk"), "rb").read()
    bolt = open(os.path.join(ROOT, "re", "sound", "wpn-rifle-rembrandt-bolt.bnk"), "rb").read()
    data = frontier_sound.build_bank(lever, bolt)
    os.makedirs(os.path.join(STAGE, "soundbank4"), exist_ok=True)
    open(os.path.join(STAGE, "soundbank4", D.FRONTIER_BANK + ".bnk"), "wb").write(data)
    for lst in (paths, order):
        if f"soundbank4/{D.FRONTIER_BANK}.bnk" not in lst:
            last = max(i for i, x in enumerate(lst) if x.startswith("gtex1/") or x.startswith("soundbank4/"))
            lst.insert(last + 1, f"soundbank4/{D.FRONTIER_BANK}.bnk")
    print(f"sound: {D.FRONTIER_BANK} rebuilt for the Frontier Rifle: own shot ({len(wav_dir)} thump layers + the hunting rifle's crack)")


def stage_p1_anims(paths, order, wa, mdata, wg):
    """T2R_P1_GUNS=1 (with T2R_FACTION_GUNS=1): the Part I Shorty / El Diablo reload, fire-gun clips
    (tools/part1/p1_wire.py, re/part1/weapons.md section 9): the carrier anim pak (its own anim events - NOT stripped),
    the two player overlay modules (pistol-uber / pistol-shotgun) and the weapon-art fields.  Needs the baked clips of
    `python tools/part1/p1_anims.py bake` (re/part1/anims in the main checkout)."""
    sys.path.insert(0, os.path.join(TOOLS, "part1"))
    import p1_wire
    data, names = p1_wire.carrier_pak()
    open(os.path.join(STAGE, "actor97", p1_wire.MP_ACTOR + ".pak"), "wb").write(data)
    p = "actor97/" + p1_wire.MP_ACTOR + ".pak"
    for lst in (paths, order):   # (runs before the model staging adds rifle-autorifle4: after the last actor97 entry then)
        if p not in lst:
            anchor = "actor97/rifle-autorifle4.pak"
            at = lst.index(anchor) + 1 if anchor in lst else max((i for i, x in enumerate(lst) if x.startswith("actor97/")), default=len(lst) - 1) + 1
            lst.insert(at, p)
    print(f"anim: Part I gun clips in {p1_wire.MP_ACTOR} ({len(data)} bytes) in the MP level")
    p1_wire.patch_art(wa, names)
    p1_wire.patch_gameplay(wg)                     # the Shorty is a SIDEARM (class 1: right hand, holster slot)
    # Part I sounds: the host sets' banks (Ruger -> Shorty, .357 -> El Diablo) rebuilt with the Part I waves
    import p1_sound
    os.makedirs(os.path.join(STAGE, "soundbank4"), exist_ok=True)
    for gun in ("shorty", "diablo"):
        name, data = p1_sound.build(gun)
        open(os.path.join(STAGE, "soundbank4", name), "wb").write(data)
        for lst in (paths, order):
            if "soundbank4/" + name not in lst:
                last = max(i for i, x in enumerate(lst) if x.startswith("gtex1/") or x.startswith("soundbank4/"))
                lst.insert(last + 1, "soundbank4/" + name)
    for rel in p1_wire.overlay_modules(os.path.join(ROOT, "re", "gamebin", "dc1"), os.path.join(STAGE, "bin", "dc1"), names):
        dc_modules.set_size(mdata, rel[:-4], os.path.getsize(os.path.join(STAGE, "bin", "dc1", *rel.split("/"))))
        f = "bin/dc1/" + rel
        for lst in (paths, order):
            if f not in lst:
                lst.insert(lst.index("bin/dc1/weapon-gameplay.bin") + 1, f)


def stage_uc4_downed(paths, order):
    """actor97/anim-san-infected.pak (+ in mode 1 actor97/anim-npc-m-horse.pak): the game's paks (cached in
    re/gamebin/actor97, else read out of sp-common.psarc) with the downed clips baked in
    (re/uc4ps4/tools/build_downed_pak.py; DOWNED.md sections 8 and 10)."""
    uc4 = os.path.join(ROOT, "re", "uc4ps4", "tools")
    sys.path.insert(0, uc4)
    import build_downed_pak
    game_pak = _game_pak

    def stage(name, data):
        _stage_anim_pak(paths, order, name, data, f"UC4 downed (mode {UC4_MODE})")

    ustats = [os.path.join(ROOT, "re", "uc4ps4")]
    data = build_downed_pak.build(game_pak(UC4_PAK), UC4_MODE)
    if not _verify_cached("uc4-downed", ["build_downed_pak"], [data, UC4_MODE], lambda: build_downed_pak.verify(data, UC4_MODE), stats=ustats):
        raise SystemExit("UC4 downed pak does not decode back to the baked clips")
    stage(UC4_PAK, data)
    if UC4_MODE == 1:       # turns, rotations, directional knock-downs, downed deaths (23 pooled carriers)
        data = build_downed_pak.build_horse(game_pak(UC4_HORSE_PAK))
        if not _verify_cached("uc4-horse", ["build_downed_pak"], [data], lambda: build_downed_pak.verify_horse(data), stats=ustats):
            raise SystemExit("UC4 horse carrier pak does not decode back to the baked clips")
        stage(UC4_HORSE_PAK, data)
        # own body: the same loops baked on the player rig (bake_player.py), the ext renames our prone clips to them
        data = build_downed_pak.build_player(game_pak(UC4_PLAYER_PAK))
        if not _verify_cached("uc4-player", ["build_downed_pak"], [data], lambda: build_downed_pak.verify_player(data), stats=ustats):
            raise SystemExit("UC4 player-rig carrier pak does not decode back to the baked clips")
        stage(UC4_PLAYER_PAK, data)


def run(*args):
    _prof.lap("(stage_modfs tail / between steps)")
    subprocess.run([sys.executable, *args], check=True, cwd=TOOLS)
    _prof.lap("pack step: " + os.path.basename(args[0]) + (" " + os.path.basename(args[3]) if len(args) > 3 and args[0] == "patch_winmm.py" else ""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--port", type=int, default=7001)
    ap.add_argument("--ext", help="also compile tools/ext/t2r-ext.c into this DLL, in the background from the moment the generated headers are final")
    a = ap.parse_args()
    s0 = int(os.environ.get("T2R_SALT0", "0"))
    _prof.reset()
    for salt in range(s0, s0 + 12):
        build(a, salt)
        _prof.lap("final dll write / cleanup")
        join_ext_compile()
        clean = defender_clean(os.path.abspath(a.out))
        _prof.lap("Defender scan")
        if clean:
            if _SLIM_PLAN is not None:
                slim_outputs(os.path.abspath(a.out))
                _prof.lap("slim: patches + slim DLL + helper check")
            _bc.prune()
            st = _bc.stats
            thumbs = sum(1 for n, _ in st["hit"] if n.startswith("map-thumb"))
            _prof.report(os.path.join(TOOLS, "local", "build_profile.log"), extra=[
                "-- build cache (tools/local/build-cache; T2R_NO_BUILD_CACHE=1 = off) --",
                f"hit {len(st['hit'])}: " + ", ".join(n for n, _ in st["hit"] if not n.startswith("map-thumb")) + f" (+{thumbs} map thumbs)",
                f"built {len(st['miss'])}: " + ", ".join(f"{n} {t:.1f}s" for n, t in st["miss"])])
            return
        print(f"Defender flags the build (ML false positive); rebuilding with salt {salt + 1}")
    raise SystemExit("Defender flagged 12 builds in a row")


def slim_outputs(out):
    """T2R_SLIM=1: next to the (unchanged, full) `out` write <out>.slim (the DLL with the patched files' chunks zeroed) and
    <out>.slim.dat (the patches); prove with the helper exe that they rebuild `out` byte for byte from the game's files."""
    import slim
    slim_dll, dat = out + ".slim", out + ".slim.dat"
    extras = []
    sid = os.path.join(ROOT, "extracted", "InstallerFiles", "tlou-ii-mp", "sidbase.bin")        # (80 MB: 30.9 MB deflated in a zip, 18.1 MB inside the xz pack)
    if os.environ.get("T2R_SLIM_PACK", "1") != "0" and os.environ.get("T2R_SLIM_EXTRAS", "1") != "0" and os.path.exists(sid):
        extras.append(("tlou-ii-mp/sidbase.bin", sid))        # the helper writes it into <game>	lou-ii-mp\; build_release then leaves it out of the client folder
    slim.emit(out, _SLIM_PLAN, slim_dll, dat, extras=extras)
    exe = slim.build_helper()
    if os.environ.get("T2R_SLIM_CHECK", "1") != "0":
        if os.environ.get("T2R_SLIM_CHECK_DEFER") == "1":
            print("slim check: left to the caller (T2R_SLIM_CHECK_DEFER=1; build_release runs it beside the zip)")
        else:
            slim.check(out, slim_dll, dat, _SLIM_PLAN["gamedir"], exe)
    print(f"slim client files: {slim_dll} ({os.path.getsize(slim_dll) / 1e6:.0f} MB, mostly zeros: zips much smaller) + {dat} ({os.path.getsize(dat) / 1e6:.0f} MB)")


def defender_clean(path):
    """True unless Windows Defender's scanner (if present) reports a threat in `path`."""
    mp = os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "Windows Defender", "MpCmdRun.exe")
    if not os.path.exists(mp):
        return True
    r = subprocess.run([mp, "-Scan", "-ScanType", "3", "-File", path, "-DisableRemediation"],
                       capture_output=True, text=True)
    return "Threat " not in r.stdout and os.path.exists(path)     # (real-time protection may delete it)


_EXT_PROC = []


def start_ext_compile(out):
    """--ext OUT: compile tools/ext/t2r-ext.c into OUT in a background gcc, started as soon as every generated tools/ext header is final
    (end of stage_modfs) and joined at the end of main(), so the compile (1-2 min) overlaps t2r_pack / winmm patches / slim. Output goes
    to OUT.tmp (renamed on success), gcc's log to OUT.log."""
    if _EXT_PROC:
        return                                             # (a Defender-flag rebuild reuses the compile)
    ext = os.path.join(TOOLS, "ext")
    cands = [os.path.join(TOOLS, "win", "ext", "msys64", "ucrt64", "bin"), os.path.join(os.environ.get("T2R_MAIN_ROOT", ROOT), "tools", "win", "ext", "msys64", "ucrt64", "bin"),
             os.path.join(os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")), "tools/win/ext/msys64/ucrt64/bin")]
    bindir = next((c for c in cands if os.path.isfile(os.path.join(c, "gcc.exe"))), None)
    if bindir is None:
        raise SystemExit("--ext: gcc.exe (msys64 ucrt64) not found")
    out = os.path.abspath(out)
    tmp = out + ".tmp"
    log = open(out + ".log", "w")
    env = dict(os.environ, PATH=bindir + os.pathsep + os.environ["PATH"])
    # tools/ext/build_ext.py: release mode = the same gcc line as before (-O2 ...); T2R_EXT_MODE=fast|dev|lto picks a quicker A/B-only build;
    # an unchanged ext source set is copied from the content cache instead of recompiled
    proc = subprocess.Popen([sys.executable, os.path.join(ext, "build_ext.py"), tmp], cwd=ext, env=env, stdout=log, stderr=subprocess.STDOUT)
    _EXT_PROC.append((proc, tmp, out, log))
    print(f"ext: gcc started in the background -> {out}")


def join_ext_compile():
    if not _EXT_PROC:
        return
    proc, tmp, out, log = _EXT_PROC[0]
    t0 = time.perf_counter()
    rc = proc.wait()
    log.close()
    if rc != 0 or not os.path.exists(tmp):
        raise SystemExit(f"--ext: gcc failed (rc {rc}), see {out}.log")
    os.replace(tmp, out)
    _prof.lap(f"ext: gcc join (waited {time.perf_counter() - t0:.1f} s; the compile ran beside the pack steps)")
    print(f"ext: {out} ({os.path.getsize(out)} bytes)")


_P1_VIRT = None


def _pack_virt(t2r_pack, finish):
    """T2R_P1_SIDE=1: split the archive (Part I data -> part1.dat) and finish the image. The payload must start `delta` bytes into a 64 KiB-aligned
    view and the cut must make (delta + T) % 64 KiB == 0 (p1_side_map.py): the file offset of the payload is a property of the finished image, so a probe
    image (tiny payload, same patch chain) tells delta, and the real image is checked against it. -> (image, side file bytes, info)"""
    base = open(BASE_DLL, "rb").read()
    rsz = t2r_pack.find_blob(base)[1].Size        # (the probe must leave the original resource like the real payload does: it moves to .t2rc, and shrink_rsrc then runs)
    probe = finish(t2r_pack.write_dll(base, t2r_pack.build_t2me(bytes(rsz + 65536))), None)
    delta = t2r_pack.payload_file_offset(probe) % 0x10000
    del probe
    for attempt in range(3):
        out, side, info = t2r_pack.pack_split_bytes(base, STAGE, _P1_VIRT["paths"], delta)
        final = finish(out, info)
        fo = t2r_pack.payload_file_offset(final)
        if (fo + info["size1"]) % 0x10000 == 0:
            print(f"p1 side: payload file offset {fo:#x}, delta {delta:#x}, cut {info['size1']:#x} (aligned), side {info['size2'] + 16} bytes, build id {info['build_id'].hex()}")
            return final, side, info
        delta = fo % 0x10000
        print(f"p1 side: payload file offset moved to {fo:#x} - re-cutting with delta {delta:#x}")
    raise SystemExit("p1 side: the payload file offset does not settle")


def _pack_cached(t2r_pack, drop):
    """t2r_pack.pack_bytes(BASE_DLL, STAGE, drop), remembered whole when T2R_DEV_PACK_CACHE=1 (A/B dev builds; default OFF): the packed image is a pure function
    of the base DLL, the staged files (content) and the drop list, so a build whose stage is unchanged (ext / winmm-patch / header edits) reads the earlier
    image instead of psarc + dsar + xor + fnv again. The two newest images are kept (1.7 GB each). Same bytes as without the cache."""
    if os.environ.get("T2R_DEV_PACK_CACHE") != "1" or _bc.DISABLED:
        return t2r_pack.pack_bytes(BASE_DLL, STAGE, drop=drop)
    key = _bc.key("devpack", _bc.File(BASE_DLL), _bc.Hashed(STAGE), _bc.Code("t2r_pack"), list(drop), os.environ.get("T2R_LZ4_LEVEL", ""), os.environ.get("T2R_FAST_PACK", ""))
    d = os.path.join(_bc.CACHE_DIR, "devpack")
    f = os.path.join(d, key + ".bin")
    if os.path.isfile(f):
        os.utime(f, None)
        print(f"build cache: devpack - hit ({key})")
        with open(f, "rb") as fh:
            return fh.read()
    out = t2r_pack.pack_bytes(BASE_DLL, STAGE, drop=drop)
    os.makedirs(d, exist_ok=True)
    tmp = f"{f}.tmp{os.getpid()}"
    with open(tmp, "wb") as fh:
        fh.write(out)
    os.replace(tmp, f)
    for old in sorted((os.path.join(d, n) for n in os.listdir(d) if n.endswith(".bin")), key=os.path.getmtime)[:-2]:
        try:
            os.remove(old)
        except OSError:
            pass
    print(f"build cache: devpack - built ({key})")
    return out


def build(a, salt):
    stage_modfs(salt)
    _prof.lap("stage_modfs: manifest write")
    if getattr(a, "ext", None):
        start_ext_compile(a.ext)
    ready = os.environ.get("T2R_EXT_READY_FILE")
    if ready:       # build_release compiles t2r-ext beside the rest of this build: every generated tools/ext header is final now
        with open(ready, "w") as f:
            f.write("ok")
    if os.environ.get("T2R_PNG_OPT", "1") != "0":       # lossless IDAT recoding of the staged PNGs (pixel-checked, cached; no-op without pyoxipng)
        import png_opt
        png_opt.optimize_stage(STAGE)
        _prof.lap("png_opt: lossless PNG recompression")
    # one in-process pipeline (was: four python processes, each reading and writing the 470 MB DLL): pack the stage into the base
    # DLL, apply the code patches, add the t2r-ext loader, retarget the server address
    import t2r_pack
    import winmm_patches
    import ext_loader
    import patch_winmm
    global _SLIM_PLAN
    _SLIM_PLAN = None
    # level paks of the stage (copies: lightmap_fix keeps its undo records next to the level-edit-cache files) get a gapless VRAM trailer BEFORE
    # the slim plan and the pack read their sizes: TextureMgrPub::ConsumeData asserts at tlou-ii.exe+12e7ee5 on a chunk that starts in dead bytes
    # (release 165u Supermarket load crash on A and B). T2R_NO_TRAILER_COMPACT=1 = off.
    if os.environ.get("T2R_TRAILER_COMPACT"):     # OFF (165x: snow_clone.Pak.serialize broke level paks, A+B crash exe+1717d03 reading offsets as pointers)
        import glob as _glob
        for _f in sorted(_glob.glob(os.path.join(STAGE, "pak68", "*.pak"))):
            with open(_f, "rb") as _fh:
                _d = _fh.read()
            _o = t2r_pack._gapless(os.path.relpath(_f, STAGE), _d)
            if _o is not _d:
                with open(_f, "wb") as _fh:
                    _fh.write(_o)
        _prof.lap("level paks: gapless texture trailers")
    if SLIM:       # T2R_SLIM=1: classify the stage against the game's own files (re/slim-build.md); identical files in always-mounted archives are left out
        import slim
        _SLIM_PLAN = slim.plan(STAGE)
        _prof.lap("slim: plan (classify vs the game's files)")
    if _P1_VIRT:
        assert not SLIM, "T2R_P1_SIDE=1 and T2R_SLIM=1 cannot be combined"
    else:
        out = _pack_cached(t2r_pack, _SLIM_PLAN["drop"] if _SLIM_PLAN else ())
    _prof.lap("pack: t2r_pack (psarc/dsar lz4/T2ME/fnv/self-check)")

    def finish(out, side_info):
        out = winmm_patches.apply(out, ["box-ingredients", "box-crowbar", "box-crowbar-ungated", "supplybox-12-cap", "supplybox-12-resize", "supplybox-12-loop", "tab-native-backpack", "mark-without-perk", "version-text", "reticle-marker-default",
            "lp-9mm-free", "lp-revolver-free", "lp-mpx5-3", "lp-crossbow-3", "lp-hawkeyed-2", "lp-marathon-1", "lp-steadyhands-4",
            "lobby8-found-loop", "lobby8-fill-test", "lobby8-clear-skip", "lobby8-count", "lobby8-search-loop", "lobby8-fill-loop"])   # find-match lobby list: 8 rows (4v4)
        out = ext_loader.add_loader(out)
        _prof.lap("pack: winmm_patches + ext_loader")
        if os.environ.get("T2R_SLOTS8", "1") != "0":       # 8 net slots (4v4): tables relocated into the .t2r8 section (re/winmm-8slots.md); T2R_SLOTS8=0 = the original 4
            import slots8
            out, _s8 = slots8.apply(out)
            print(f"slots8: {len(_s8.report)} sites, cave {len(_s8.cave.code):#x} bytes, data {_s8.dsize:#x} bytes")
            _prof.lap("pack: slots8 (8-player winmm)")
        import stubpool        # reserved executable stub pool inside the image: the ext's stubs must be within rel32 of winmm code (re/stub-pool-1103.md)
        out, _sp_rva, _sp_vs = stubpool.add(out)
        stubpool.check(out)
        print(f"stubpool: .t2rstub rva {_sp_rva:#x} size {_sp_vs:#x}")
        if _P1_VIRT:        # the side-file cave (p1_side_map.py): maps part1.dat behind the blob at the loader; a probe image (side_info None) only needs the layout
            import p1_side_map
            out = p1_side_map.add(out, side_info["size2"] if side_info else 1, side_info["build_id"] if side_info else bytes(16))
            p1_side_map.check(out)
        ips, ports = patch_winmm.current_target(out)
        if len(ips) != 1 or len(ports) != 1:
            raise SystemExit(f"unexpected bytes at patch sites: {ips} {ports} (not a T2R winmm.dll?)")
        out = patch_winmm.patch(out, "127.0.0.1", a.port)
        if os.environ.get("T2R_SHRINK_RSRC", "1") != "0":       # drop the dead 162 MB zeroed RCDATA from the file (virtual layout unchanged)
            import shrink_rsrc
            n0 = len(out)
            out = shrink_rsrc.shrink(out)
            print(f"shrink_rsrc: {n0} -> {len(out)} bytes")
        return out

    side = None
    if _P1_VIRT:
        out, side, _sinfo = _pack_virt(t2r_pack, finish)
    else:
        out = finish(out, None)
    ips, ports = patch_winmm.current_target(out)
    with open(os.path.abspath(a.out), "wb") as f:
        f.write(out)
    print(f"{a.out}: {ips.pop()}:{ports.pop()} -> {patch_winmm.current_target(out)}")
    if _P1_VIRT:
        p1_side.write_virt_outputs(p1_side.out_dir(ROOT), _P1_VIRT["entries"], _P1_VIRT["kept"], side)
        print(f"p1 side: deliver {a.out} + {p1_side.out_dir(ROOT)}/part1.dat + manifest.txt (launcher: <game dir>/t2r-maps/part1/); self-test: re/part1/side-selftest.md")


if __name__ == "__main__":
    main()
