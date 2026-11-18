"""Build the shareable package: dist/TwelveWeeks/ and dist/TwelveWeeks.zip.

  python release/build_release.py [--server host:port] [--sp-maps hillcrest-stores,lodge | all] [--no-cache]

SP-section maps (tools/maps/sp_maps.json) are built into the client only when listed with --sp-maps; the default is NONE. An
inherited T2R_SP_MAPS from the calling shell is ignored (removed from the environment of every build step), so a dev shell can't
leak maps into a release. The list is recorded in client/sp-maps.txt, in the build log and at the end of CHANGES.txt.

Build time: every step prints "step <name>: N.N s" and the build ends with the whole profile. The data build
(tools/features/build_dll.py) uses the content-hash step cache (tools/buildcache.py, tools/local/build-cache), the t2r-ext compile
is cached by the hash of its sources and overlaps with the DLL packing, the slim helper's rebuild check overlaps with the zip, and
the host folder is prepared while the data build runs. The output is byte-identical with everything off: --no-cache runs every
step for real (step cache, ext compile cache, lz4 chunk cache, slim-check marker).

Layout:
  client/  winmm.dll (features client, pre-pointed at 127.0.0.1 so a direct game launch
           never dials an old server address), t2r-ext.dll, version.txt, tlou-ii-mp/, T2R-Play.ps1,
           Play.bat (install + play, no setup step), server.txt (default server address),
           steam/ (t2r-steam.exe: Steam name + "Playing Spacewar" presence)
  host/    daemon.py, T2R-Host.ps1, Start Server.bat
  README.txt, PLAYIT-GUIDE.txt
"""
import argparse
import concurrent.futures
import contextlib
import hashlib
import re
import subprocess
import os
import shutil
import sys
import tempfile
import threading
import time
import zipfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import patch_winmm  # noqa: E402
import t2rtmp  # noqa: E402  (T2R_TMP, named temp files, cleanup at exit: tools/t2rtmp.py)

# What the host folder carries. The daemon imports these at start (a missing one = ImportError, the server does not start). tests/test_release_host.py imports the daemon from a folder holding exactly this list.
HOST_FILES = ("daemon.py", "clan_journal_entries.py", "gen_modes.py", "modeq.py", "rankbadge.py", "names.py", "clan.py", "hostmig.py", "hostcap.py", "map_packs.py", "interrogation.py", "interrogation_lockboxes.json", "gunlimit.py", "notices.py", "anticheat.py",
              "clan_story.py", "clan_story_data.py", "clan_story_lines_good.py", "clan_story_lines_dark.py",
              "clan_story_pl.py", "clan_story_pl_good.py", "clan_story_pl_dark.py", "clan_story_solo.py", "clan_story_good_rich.py",
              "clan_alife.py", "clan_alife_lex.py", "clan_alife_text_a.py", "clan_alife_text_b.py", "clan_alife_text_c.py", "clan_alife_text_d.py", "clan_alife_text_e.py", "clan_alife_text_f.py", "clan_journal.py", "clan_journal_text.py", "clan_journal_text_pl.py", "requeue.py", "clan_reset.py",
              # clan chronicle / saga / arcs / supply modules the daemon imports (found by tools/tests/test_release_host.py's import-graph walk)
              "clan_arcs.py", "clan_arcs_text.py", "clan_arcs_text2.py", "clan_story_spores.py", "clan_chronicle.py", "clan_status.py", "clan_perks.py", "clan_kids.py", "clan_continuity.py", "clan_ingame.py", "social.py", "clan_portraits.py", "clan_portrait_pool.py", "clan_chronicle_maps.py",
              "clan_chronicle_text.py", "clan_saga.py", "clan_saga_data.py", "clan_saga_pl.py",
              "clan_saga_pl_data.py", "clan_saga_pl_text.py", "clan_saga_pl_text2.py", "clan_saga_pl_text3.py",
              "clan_saga_pl_text4.py", "clan_saga_pl_text_sup.py", "clan_saga_text.py", "clan_saga_text2.py", "clan_saga_text3.py",
              "clan_saga_text4.py", "clan_saga_text_sup.py", "clan_supply.py",
              "clan_loot.py", "clan_loot_data.py", "clan_loot_data2.py")      # (clan_loot: the rich findings of a raid)      # (clan_reset: admin clan wipe + backup/restore; clan_story imports the Polish twin: without it the feed is silently off; the clan A-Life (switch tools/clan_alife.txt, default off) is optional but ships)
OPTIONAL_HOST_FILES = ("anticheat.py",)   # copied when present; the host runs without it
# Members every player zip must hold (paths inside the zip). Slim builds hold winmm.dll without the data the game provides.
SHIP_SP_MAPS = "train-station,hospital-ellie,wilds-houses,capitol-shops,into-the-city,overpass,jackson-market"   # the SP-section maps every PT ships (Bookstore parked)
ZIP_REQUIRED = ("client/winmm.dll", "client/t2r-ext.dll", "client/version.txt", "client/server.txt", "client/sp-maps.txt", "client/Play.bat",
                "client/T2R-Play.ps1", "client/steam/t2r-steam.exe", "client/steam/steam_api64.dll", "client/tlou-ii-mp/sidbase.bin",
                "client/tlou-ii-mp/t2r-looks.txt")
ORIG_DLL = os.path.join(ROOT, "extracted", "InstallerFiles", "winmm.dll")
ORIG_DATA = os.path.join(ROOT, "extracted", "InstallerFiles", "tlou-ii-mp")
OUT = os.path.join(ROOT, "dist", "TwelveWeeks")

# ---------------------------------------------------------------------------------------------------- per-step timing
_STEPS = []                    # (name, seconds, background)
_STEPS_LOCK = threading.Lock()


def _book(name, dt, bg):
    with _STEPS_LOCK:
        _STEPS.append((name, dt, bg))
    print("step %s: %.1f s%s" % (name, dt, " (background)" if bg else ""), flush=True)


@contextlib.contextmanager
def step(name, bg=False):
    t = time.perf_counter()
    try:
        yield
    finally:
        _book(name, time.perf_counter() - t, bg)


def _profile(wall):
    print("\n=== release build profile (wall %.1f s) ===" % wall)
    print("%-60s %8s" % ("step", "sec"))
    for n, dt, bg in _STEPS:
        print("%-60s %8.1f%s" % (n[:60], dt, "   (background, overlaps the steps around it)" if bg else ""))
    print("%-60s %8.1f" % ("TOTAL wall", wall), flush=True)


# ---------------------------------------------------------------------------------------------------- t2r-ext compile
def _gcc_paths():
    d = os.path.join(ROOT, "tools", "win", "ext", "msys64", "ucrt64", "bin")
    return d, os.path.join(d, "gcc.exe")


EXT_FLAGS = ["-std=gnu2x", "-O2", "-s", "-shared", "-static", "-DT2R_RELEASE", "-o", "t2r-ext.dll", "t2r-ext.c"]


def ext_key(ext_dir, gcc, env, no_cache):
    """sha256 of everything the release ext compile reads: every file of tools/ext (sources, generated headers, embedded pcm data;
    not the DLLs / objects it writes), the flags and the compiler (gcc.exe + its version / cc1 / ld)."""
    import buildcache
    h = hashlib.sha256()
    h.update(" ".join(EXT_FLAGS).encode())
    for arg in (["-dumpfullversion"], ["-dumpmachine"], ["-print-prog-name=cc1"], ["-print-prog-name=ld"]):
        r = subprocess.run([gcc] + arg, capture_output=True, text=True, env=env)
        out = r.stdout.strip()
        h.update(b"\0" + out.encode())
        if os.path.isfile(out):
            st = os.stat(out)
            h.update(b"%d:%d" % (st.st_size, st.st_mtime_ns))
    st = os.stat(gcc)
    h.update(b"%d:%d" % (st.st_size, st.st_mtime_ns))
    for base, dirs, files in os.walk(ext_dir):
        dirs[:] = sorted(d for d in dirs if d != "__pycache__")
        for f in sorted(files):
            if f.endswith((".dll", ".o", ".exe", ".log", ".pyc")):
                continue
            p = os.path.join(base, f)
            h.update(b"\0" + os.path.relpath(p, ext_dir).encode() + b"=" + buildcache._hash_file(p).encode())
    return h.hexdigest()[:32]


def build_ext(no_cache):
    """tools/ext/t2r-ext.dll, the release build (-DT2R_RELEASE: no test flags/keys), whatever the dev build is. Recompiled only when
    its sources / compiler changed (cache: tools/local/build-cache/ext/<key>.dll; --no-cache compiles every time)."""
    ext_dir = os.path.join(ROOT, "tools", "ext")
    gdir, gcc = _gcc_paths()
    gcc_env = dict(os.environ, PATH=gdir + os.pathsep + os.environ["PATH"])
    dll = os.path.join(ext_dir, "t2r-ext.dll")
    cdir = os.path.join(ROOT, "tools", "local", "build-cache", "ext")
    t0 = time.perf_counter()
    key = None if no_cache else ext_key(ext_dir, gcc, gcc_env, no_cache)
    hit = os.path.join(cdir, key + ".dll") if key else None
    if hit and os.path.isfile(hit):
        shutil.copyfile(hit, dll)
        os.utime(hit, None)
        _book("t2r-ext compile: cache hit (%s)" % key[:8], time.perf_counter() - t0, True)
        return dll
    subprocess.run([gcc] + EXT_FLAGS, cwd=ext_dir, env=gcc_env, check=True)
    if hit:
        try:
            os.makedirs(cdir, exist_ok=True)
            tmp = "%s.%d.tmp" % (hit, os.getpid())
            shutil.copyfile(dll, tmp)
            os.replace(tmp, hit)
            olds = sorted((os.path.join(cdir, f) for f in os.listdir(cdir) if f.endswith(".dll")), key=os.path.getmtime, reverse=True)
            for p in olds[8:]:             # (50 MB each: keep the newest eight)
                os.remove(p)
        except OSError:
            pass
    _book("t2r-ext compile (gcc -O2)", time.perf_counter() - t0, True)
    return dll


# ---------------------------------------------------------------------------------------------------- slim rebuild check
def _sha_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 22), b""):
            h.update(b)
    return h.hexdigest()


def slim_check_run(full, slim_dll, dat, no_cache):
    """The helper rebuilds the full DLL from the slim one (+ .dat + the game's files) and it must be byte-identical (slim.check).
    A pass is remembered by the content of the three files, the helper exe and the game's archives (name, size, mtime): the same
    inputs are not proven twice. Runs in a worker thread; the swap-in waits for it."""
    t0 = time.perf_counter()
    sys.path.insert(0, os.path.join(ROOT, "tools", "features"))
    import slim
    exe = os.path.join(ROOT, "release", "client", "slim", "t2r-slim.exe")
    gamedir = slim.game_dir()
    marker = None
    if not no_cache:
        # (the helper exe itself is not hashed: every compile stamps a new PE timestamp into it; its source + the compiler stand for it)
        src_c = os.path.join(ROOT, "release", "client", "slim", "t2r-slim.c")
        with concurrent.futures.ThreadPoolExecutor(4) as ex:
            hs = list(ex.map(_sha_file, [full, slim_dll, dat, src_c]))
        st = os.stat(_gcc_paths()[1])
        hs.append("gcc:%d:%d" % (st.st_size, st.st_mtime_ns))
        main_dir = os.path.join(gamedir, *slim.MAIN.split("/"))
        rows = []
        for f in sorted(os.listdir(main_dir)):
            st = os.stat(os.path.join(main_dir, f))
            rows.append("%s:%d:%d" % (f, st.st_size, st.st_mtime_ns))
        key = hashlib.sha256((" ".join(hs) + "|" + "\n".join(rows) + "|v1").encode()).hexdigest()[:32]
        marker = os.path.join(ROOT, "tools", "local", "build-cache", "slim-check", key)
        if os.path.exists(marker):
            os.utime(marker, None)
            _book("slim check: passed before for these exact files (%s)" % key[:8], time.perf_counter() - t0, True)
            return
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "features", "slim.py"), "check", full, slim_dll, dat],
                       capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    if r.returncode:
        raise SystemExit("slim check failed (rc %d):\n%s%s" % (r.returncode, r.stdout[-800:], r.stderr[-800:]))
    if marker:
        os.makedirs(os.path.dirname(marker), exist_ok=True)
        open(marker, "w").write("ok")
    _book("slim check (helper rebuild + cmp)", time.perf_counter() - t0, True)


def preflight(a):
    """Fail in seconds, listing EVERY missing build input, instead of after a 40 minute data build (for example a
    missing weapon-art.bin in a fresh worktree). Git-ignored inputs only exist in the main checkout: python tools/win/prep_worktree.py links / copies them."""
    j = lambda *p: os.path.join(ROOT, *p)
    need = [j("tools", "local", "build-cache"), j("tools", "local", "vgmstream"), j("re", "models"), j("re", "modfs"), j("re", "sound"),
            j("re", "gamebin", "dc1", "weapon-art.bin"), j("re", "maps", "paks"), j("re", "maps", "seeds"), j("re", "uc4ps4", "paks"),
            j("release", "client", "steam", "steam_api64.dll"), j("release", "client", "steam", "t2r-steam.exe"), j("release", "client", "slim", "t2r-slim.c"),
            j("tlou2_pak_editor", "build", "gcc", "pak_editor_cli.exe")]
    need += [j("tools", "features", "data", "wedge", n) for n in ("weapon-art.bin", "attach-points.bin", "custom-attach-points.bin", "player-anim-settings.bin",
                                                                     "player-overlays.bin", "ai-weapon-skills.bin", "look2", "motion-matching-settings",
                                                                     "player-specific-weapon-overlays")]
    need += [j("re", "uc4ps4", "t2", n + ".pak") for n in ("anim-melee-abby-t2", "anim-melee-player-shared-t2", "base-female-skel")]
    need += [j("re", "factions2013", "t1anim", "paks", n) for n in ("anim-joel-net-1.00.pak", "anim-melee-common.pak", "anim-melee-dlc5.pak", "anim-melee-net-1.00.pak",
                                                                      "anim-melee-net-patch.pak", "anim-net-revive.pak", "joel-skel.pak", "net-melee-patch.bin")]
    t2 = os.environ.get("T2_GAME", "C:/Games/The Last of Us Part II Remastered")
    need += [os.path.join(t2, "build", "pc", "main"), os.environ.get("UC4_ELF", "E:/Games/WP/uc4/uroot/big4-mp.elf")]
    if a.p1_side or any(k.strip().startswith("p1-") for k in a.sp_maps.split(",")):
        need += [j("re", "part1", n) for n in ("game-path.txt", "game-files.tsv", "texdict-index.tsv", "archives-p1.tsv")]
        try:
            need.append(os.path.join(open(j("re", "part1", "game-path.txt")).read().strip(), "build", "pc", "main"))
        except OSError:
            pass
        for k in ("T2R_P1_CONV", "T2R_P1_AUDIO_DIR"):
            if os.environ.get(k):
                need.append(os.environ[k])
        if a.p1_side and not os.environ.get("T2R_P1_CONV"):
            need.append("env T2R_P1_CONV (--p1-side)")
    bad = [x for x in need if not (x.startswith("env ") or os.path.exists(x))] + [x for x in need if x.startswith("env ")]
    if bad:
        raise SystemExit("PREFLIGHT FAILED: %d missing build input(s):\n  %s\nrun: python tools/win/prep_worktree.py (links the git-ignored inputs from the main checkout)"
                         % (len(bad), "\n  ".join(bad)))
    lec = j("re", "maps", "level-edit-cache", "src")
    print("preflight: %d inputs present; level-edit-cache %s" % (len(need), "warm" if os.path.isdir(lec) and os.listdir(lec) else "COLD (first build re-runs every level / lightmap pass: tens of minutes)"))


def main():
    wall0 = time.perf_counter()
    wall_start = time.time()
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default="", help="default server address written to client/server.txt")
    ap.add_argument("--version", default="", help="version stamp (default: git short hash)")
    ap.add_argument("--out", default=OUT, help="output folder (deleted and rebuilt; the zip goes next to it)")
    # Builds made without --sp-maps shipped = Train Station only, Hillside / Capitol Hill /
    # Into the City / Overpass silently left the pool. The default is the shipping set; "none" builds Train Station only.
    ap.add_argument("--sp-maps", default=SHIP_SP_MAPS, help="single-player-section maps to build in: keys (comma separated), 'all' or 'none'; default the shipping set " + SHIP_SP_MAPS)
    ap.add_argument("--p1-side", action="store_true", help="Part I maps (p1-*) in --sp-maps as DLC side data: the Part I-derived files are NOT in winmm.dll but in part1.dat "
                    "(T2R_P1_SIDE=1, needs T2R_P1_CONV), staged at <out>/part1/t2r-maps/part1/part1.dat for the server download. Forces a FULL client (T2R_SLIM is dropped: "
                    "the side-file cave and the slim helper cannot be combined); every other compression step stays")
    ap.add_argument("--max-zip-mb", type=int, default=0, help="fail when the player zip is bigger; 0 = no limit (over 1 GB is fine, compress everything first)")
    ap.add_argument("--no-cache", action="store_true", help="full clean build: no step cache, no t2r-ext compile cache, no slim-check marker, no lz4 chunk cache "
                    "(same output as a cached build; use it for a public release or to prove a cached build)")
    ap.add_argument("--fast-pack", type=int, nargs="?", const=6, default=0, metavar="THREADS",
                    help="slim client: compress the .dat's solid xz pack stream as independent 96 MiB blocks on THREADS threads (default 6; ~4 min -> ~1 min, "
                    "the .dat gets a few MB bigger and DIFFERS from the default build's). Off by default: the default output is byte-identical to earlier builds")
    a = ap.parse_args()
    if os.environ.get('T2R_PREFLIGHT_ONLY') or not os.environ.get('T2R_NO_PREFLIGHT'):
        preflight(a)
    if os.environ.get('T2R_PREFLIGHT_ONLY'):
        raise SystemExit(0)
    if a.fast_pack > 1:
        os.environ["T2R_SLIM_XZ_THREADS"] = str(a.fast_pack)
    if a.no_cache:
        os.environ["T2R_NO_BUILD_CACHE"] = "1"
        os.environ["T2R_PACK_CACHE"] = "0"
    no_cache = bool(os.environ.get("T2R_NO_BUILD_CACHE"))
    # The shipped feature set (the release command line). A build made without these shipped flat TLOU2 prone downs and
    # missing faction models (the switches also bake data into winmm.dll). They are the defaults now; an explicit value in the
    # environment still wins (e.g. T2R_FLAME=1).
    for k, v in (("T2R_EXPBOW", "0"), ("T2R_FLAME", "0"), ("T2R_UC4_DOWNED", "1"), ("T2R_FACTION_LOOKS", "1"), ("T2R_FACTION_GUNS", "1"),
                 ("T2R_P1_GUNS", "1"), ("T2R_PISTOL_THIGH_SLOT1", "1"), ("T2R_SP_REUSE_SLOTS", "1"), ("T2R_SP_EDGES", "chain"),
                 # winter Supermarket (none of these had a default, a bare release shipped no snow / an over-size image)
                 ("T2R_MP_SNOW", "1"), ("T2R_MP_SNOW_XPAK", "1"), ("T2R_MP_SNOW_XPAK_UNSAFE", "1"), ("T2R_AUTO_VIS_MAPS", "jackson-market")):
        if k not in os.environ:
            os.environ[k] = v
    print("release feature switches:", " ".join("%s=%s" % (k, os.environ[k]) for k in ("T2R_SLIM", "T2R_EXPBOW", "T2R_FLAME", "T2R_UC4_DOWNED",
          "T2R_FACTION_LOOKS", "T2R_FACTION_GUNS", "T2R_P1_GUNS", "T2R_PISTOL_THIGH_SLOT1", "T2R_SP_REUSE_SLOTS", "T2R_SP_EDGES") if k in os.environ))
    print("release build: %s" % ("NO CACHE (full clean build)" if no_cache else "step cache on (tools/local/build-cache; --no-cache = full clean build)"))
    os.environ["T2R_JOEL_MM"] = "0"                        # Joel locomotion ships only with the Part I side-loader
    os.environ.pop("T2R_FAST_PACK", None)
    if a.p1_side:
        if not os.environ.get("T2R_P1_CONV"):
            raise SystemExit("--p1-side needs T2R_P1_CONV (the converted Part I folder)")
        os.environ["T2R_P1_SIDE"] = "1"
        os.environ.pop("T2R_SLIM", None)                   # (build_dll asserts: the side-file cave and slim cannot be combined)
        os.environ.pop("T2R_P1_SIDE_OUT", None)            # (default tools/local/p1side: the staging below reads it)
        print("release build: --p1-side: FULL client (no slim), Part I data goes to part1.dat, never into winmm.dll")
    else:
        os.environ.pop("T2R_P1_SIDE", None); os.environ.pop("T2R_P1_CONV", None)
    # the mod data pack in a file beside winmm.dll (tools/features/pack_file.py): the image stays ~170 MB whatever the content, far from the
    # Windows loader's ~2,000,000,000-byte edge. Default ON; T2R_PACK_FILE=0 = the old in-image pack. A slim client keeps the old pack.
    if os.environ.get("T2R_SLIM") == "1":
        os.environ["T2R_PACK_FILE"] = "0"
    os.environ.setdefault("T2R_PACK_FILE", "1")
    pack_file_on = os.environ["T2R_PACK_FILE"] == "1"
    print("release build: mod data %s" % ("in client/t2r-maps/main/t2r-data.bin (T2R_PACK_FILE=1)" if pack_file_on else "inside winmm.dll (T2R_PACK_FILE=0)"))
    inherited = os.environ.pop("T2R_SP_MAPS", None)        # never inherited: only --sp-maps decides
    os.environ.pop("T2R_P1_LEVELS", None)                  # (isolation-build switch: never in a release)
    with step("setup (sp-maps check, staging folders, git stamp)"):
        sys.path.insert(0, os.path.join(ROOT, "tools", "features"))
        import sp_maps_build  # noqa: E402
        sp_sel = [m["key"] for m in sp_maps_build.selected("0" if a.sp_maps in ("", "none") else a.sp_maps)]      # validates the keys (unknown = error)
        _p1 = [m["key"] for m in sp_maps_build.selected("0" if a.sp_maps in ("", "none") else a.sp_maps) if m.get("part1") or m["key"].startswith("p1-")]
        if _p1 and not a.p1_side:
            raise SystemExit("release with Part I maps %s: Part I maps are DLC, never inside a release (use --p1-side: side file for the launcher download)" % _p1)
        if a.p1_side and not _p1:
            raise SystemExit("--p1-side without a p1-* map in --sp-maps")
        sp_env = ",".join(sp_sel)
        print("release build: SP-section maps: %s%s" % (sp_env or "none",
              " (ignored inherited T2R_SP_MAPS=%r)" % inherited if inherited else ""))
        final = os.path.abspath(a.out)
        # Built next to the final folder and swapped in only when everything (files, checks, zip) went through: a failed build leaves the
        # previous output and zip untouched (it used to rmtree the folder first and leave a half build / an old zip beside it).
        out = final + ".building"
        if os.path.exists(out):
            shutil.rmtree(out)
        stamp_zip = os.path.join(os.path.dirname(final), "factionsii.zip")   # always this name
        client, host = os.path.join(out, "client"), os.path.join(out, "host")
        os.makedirs(os.path.join(client, "tlou-ii-mp"))
        os.makedirs(host)
        stamp = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
        # Play.bat / the launcher reinstall the mod only when version.txt changes: two different builds of one commit would look equal to a
        # player. A tracked-file change is marked in the stamp (an explicit --version is used as given).
        dirty = subprocess.run(["git", "-C", ROOT, "status", "--porcelain", "--untracked-files=no"], capture_output=True, text=True).stdout.strip()
        if not a.version and dirty:
            stamp += "-d" + time.strftime("%m%d%H%M")      # (a time, so successive dirty builds differ too)
            print("WARNING: tracked files have uncommitted changes: version stamp %s (pass --version to name this build)" % stamp)
        if not stamp and not a.version:
            raise SystemExit("no git stamp and no --version: refusing to build an unversioned client")
        version = a.version or stamp
        # the host folder carries the same stamp: the daemon refuses clients of another build (version check, op 0xB0)
        open(os.path.join(host, "version.txt"), "w").write(version + "\n")
    pool = concurrent.futures.ThreadPoolExecutor(max_workers=4)
    host_job = pool.submit(prepare_host, out, host, sp_env)          # (independent of the data build: runs beside it)

    # features client: unpatched winmm.dll + feature patches/scripts (tools/features/build_dll.py)
    # + t2r-ext.dll, pointed at 127.0.0.1:7001 (Play.ps1 rewrites the address per server.txt)
    feat = os.path.join(ROOT, "tools", "local", "winmm-features-release.dll")
    slim_on = os.environ.get("T2R_SLIM") == "1"
    ready = t2rtmp.own(os.path.join(tempfile.gettempdir(), "t2r-tmp-ext-ready-%d" % os.getpid()))      # (removed at exit, also after a failed build)
    if os.path.exists(ready):
        os.remove(ready)
    env = dict(os.environ, T2R_SP_MAPS=sp_env, T2R_EXT_READY_FILE=ready, T2R_SLIM_CHECK_DEFER="1")
    proc = subprocess.Popen([sys.executable, os.path.join(ROOT, "tools", "features", "build_dll.py"), feat, "--port", "7001"], env=env)
    ext_job = pool.submit(_ext_when_ready, ready, proc, no_cache)
    with step("data build (tools/features/build_dll.py: stage, pack, patches, slim)"):
        rc = proc.wait()
    if rc:
        raise subprocess.CalledProcessError(rc, proc.args)
    import pefile
    _soi = pefile.PE(feat, fast_load=True).OPTIONAL_HEADER.SizeOfImage
    print("winmm SizeOfImage 0x%x" % _soi)
    data_file = None
    if pack_file_on:
        data_file = os.path.splitext(feat)[0] + "-t2r-data.bin"        # (= build_dll.pack_file_path)
        if not os.path.isfile(data_file) or os.path.getmtime(data_file) < wall_start:
            raise SystemExit("T2R_PACK_FILE: %s was not written by this build" % data_file)
        print("mod data file %s: %d bytes" % (data_file, os.path.getsize(data_file)))
    if _soi >= 0x71A4E000:      # largest image proven to load (re: winmm-image-limit); 0x79ED5000 failed with 0xC000007B
        raise SystemExit("winmm SizeOfImage 0x%x is over the proven-loadable 0x71A4E000: the game may not start" % _soi)
    with step("payload audit (the finished full DLL's archive read back: suppressed-rifle carrier bank + MP level adds)"):
        # A past build shipped silent suppressed rifles with every staging check green: this reads the FINAL packed bytes the way the game does
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "features", "payload_audit.py"), data_file or feat], capture_output=True, text=True)
        print((r.stdout + r.stderr).strip()[-1500:])
        if r.returncode:
            raise SystemExit("payload audit failed (see above): the build would ship broken data")
    if slim_on:
        # slim client (re/slim-build.md): winmm.dll without the data the player's own game files provide + t2r-slim.dat (patches);
        # Play.bat runs slim/t2r-slim.exe once per version to write the full winmm.dll into the game folder. Windows only.
        with step("copy slim winmm.dll / .dat / helper into the client"):
            shutil.copy(feat + ".slim", os.path.join(client, "winmm.dll"))
            shutil.copy(feat + ".slim.dat", os.path.join(client, "t2r-slim.dat"))
            os.makedirs(os.path.join(client, "slim"))
            shutil.copy(os.path.join(ROOT, "release", "client", "slim", "t2r-slim.exe"), os.path.join(client, "slim"))
        # build_dll left the rebuild check to us (T2R_SLIM_CHECK_DEFER): it runs beside the ext wait, the client assembly and the zip
        chk_job = pool.submit(slim_check_run, feat, feat + ".slim", feat + ".slim.dat", no_cache) if os.environ.get("T2R_SLIM_CHECK", "1") != "0" else None
    else:
        with step("copy winmm.dll into the client"):
            shutil.copy(feat, os.path.join(client, "winmm.dll"))
            if data_file:      # the launcher copies client/t2r-maps/** into the game folder (by size + first/last 64 KB; the build id is in both)
                os.makedirs(os.path.join(client, "t2r-maps", "main"))
                shutil.copy(data_file, os.path.join(client, "t2r-maps", "main", "t2r-data.bin"))
        chk_job = None
    with step("wait for the t2r-ext dll (compiled beside the data build)"):
        ext_dll = ext_job.result()
        if ext_dll is None:                      # (the data build never signalled: compile now)
            ext_dll = build_ext(no_cache)
    shutil.copy(ext_dll, client)
    with step("assemble the client folder"):
        open(os.path.join(client, "version.txt"), "w").write(version + "\n")
        if os.environ.get("T2R_UC4_DOWNED") == "1":     # ext flag: use the UC4 downed clips baked into the pak
            open(os.path.join(client, "t2r-uc4-downed.txt"), "w").write("on\n")
            open(os.path.join(client, "t2r-uc4-own.txt"), "w").write("on\n")       # own downed body = the UC4 crawl (else TLOU2's prone)
        if os.environ.get("T2R_FACTION_LOOKS") == "1":  # ext flag: WLF / Seraphite player models (baked into the pak by build_dll)
            open(os.path.join(client, "t2r-faction-looks.txt"), "w").write("on\n")
        if os.environ.get("T2R_FACTION_GUNS") == "1":   # ext flag: El Diablo / Shorty rows (default off)
            open(os.path.join(client, "t2r-faction-guns.txt"), "w").write("on\n")
        if os.environ.get("T2R_MELEE_DUEL", "1") != "0":   # ext flag (opt-in in the ext): the SP-style 1v1 melee duel ships ON; T2R_MELEE_DUEL=0 = a build without it
            open(os.path.join(client, "t2r-melee-duel.txt"), "w").write("on\n")
        if os.environ.get("T2R_FLASHLIGHT_PUPPETS", "1") != "0":   # ext flag (opt-in): flashlight strap light + strap model on other players (flashlight.c fls_puppets_tick)
            open(os.path.join(client, "t2r-flashlight-puppets.txt"), "w").write("on\n")
        if os.environ.get("T2R_NO_FLASHLIGHT", "1") != "0":   # ext opt-out (flashlight.c fls_off): flashlights OFF for now (the light blinds the view
            open(os.path.join(client, "t2r-no-flashlight.txt"), "w").write("on\n")   # instead of helping - fog / exposure); T2R_NO_FLASHLIGHT=0 = back on
        sid_in_dat = False
        if slim_on:
            # a slim build's .dat may carry sidbase.bin (xz: 18 MB instead of 31 MB deflated in the zip); t2r-slim.exe then writes it into the
            # game folder, and it must not ride in the zip a second time. The record table is at the start of the .dat.
            with open(os.path.join(client, "t2r-slim.dat"), "rb") as f:
                sid_in_dat = b"tlou-ii-mp/sidbase.bin" in f.read(4 << 20)
        for f in ("sidbase.bin", "t2r-looks.txt"):
            if f == "sidbase.bin" and sid_in_dat:
                continue
            shutil.copy(os.path.join(ORIG_DATA, f), os.path.join(client, "tlou-ii-mp", f))
        for f in ("T2R-Play.ps1", "Play.bat", "t2r-linux.py", "LINUX.txt"):
            shutil.copy(os.path.join(ROOT, "release", "client", f), client)
        # Steam name + Spacewar presence helper (genuine Valve steam_api64.dll, appid 480)
        os.makedirs(os.path.join(client, "steam"))
        for f in ("t2r-steam.exe", "steam_api64.dll", "steam_appid.txt"):
            shutil.copy(os.path.join(ROOT, "release", "client", "steam", f), os.path.join(client, "steam"))
        open(os.path.join(client, "server.txt"), "w").write(a.server + "\n")
        open(os.path.join(client, "sp-maps.txt"), "w").write((sp_env or "none") + "\n")
    with step("wait for the host folder (copy + import checks, ran beside the data build)"):
        host_job.result()

    # the player zip needs a server address: Play.bat stops with "No server address" without one (server.txt is empty without --server)
    if a.p1_side:
        import hashlib
        side_dir = os.path.join(ROOT, "tools", "local", "p1side")
        src = os.path.join(side_dir, "part1.dat")
        if not os.path.isfile(src) or os.path.getmtime(src) < wall_start:
            raise SystemExit("--p1-side: %s was not written by this build" % src)
        dst_dir = os.path.join(out, "part1", "t2r-maps", "part1")
        os.makedirs(dst_dir)
        dat = os.path.join(dst_dir, "part1.dat")
        shutil.copy(src, dat)
        with open(dat, "rb") as f:
            raw = f.read()
        os.makedirs(os.path.join(out, "part1", "pack"))
        shutil.copy(src, os.path.join(out, "part1", "pack", "part1.dat"))
        info = ["build version %s" % version, "build id (last 16 bytes of part1.dat) %s" % raw[-16:].hex(), "size %d" % len(raw),
                "sha256 %s" % hashlib.sha256(raw).hexdigest(),
                "publish: python tools/update_server/publish_part1.py <this build>/part1/pack --data <update data dir> --pack part1-%s" % version,
                "(part1.dat at the pack root; the launcher writes it to <game>/t2r-maps/part1/part1.dat. part1/t2r-maps/part1/part1.dat is the same file in the game layout)"]
        open(os.path.join(out, "part1", "part1.dat.info.txt"), "w").write(chr(10).join(info) + chr(10))
        print("part1.dat staged: %s (%.1f MB), build id %s" % (dat, len(raw) / 1e6, raw[-16:].hex()))
    srv = open(os.path.join(client, "server.txt")).read().strip()
    if not re.fullmatch(r"[^:\s]+:\d{1,5}", srv):
        print("WARNING: client/server.txt is %r (no --server host:port given): players cannot connect until it is filled in" % srv)
    if not slim_on and not a.p1_side:
        print("WARNING: T2R_SLIM is not 1: this is the FULL client (about 900 MB zipped); the Windows player download is built with T2R_SLIM=1")

    # the zip holds ONLY the client folder (the host folder stays local, not sent to players); written under a temp name
    tmpz = stamp_zip + ".part"
    with step("zip (deflate 9, big members compressed in parallel)"):
        write_zip(tmpz, client, out)
        with zipfile.ZipFile(tmpz) as z:
            names = set(z.namelist())
    required = [m for m in ZIP_REQUIRED if not (sid_in_dat and m == "client/tlou-ii-mp/sidbase.bin")] + (["client/t2r-slim.dat"] if sid_in_dat else [])         + (["client/t2r-maps/main/t2r-data.bin"] if data_file else [])
    missing = [m for m in required if m not in names]
    size = os.path.getsize(tmpz)
    if missing or (a.max_zip_mb and size > a.max_zip_mb * 1_000_000):
        os.remove(tmpz)
        raise SystemExit("zip refused (%s): missing %s, size %.0f MB (limit %d MB). Nothing was swapped in." % (stamp_zip, missing or "nothing", size / 1e6, a.max_zip_mb))
    if chk_job is not None:
        with step("wait for the slim rebuild check"):
            try:
                chk_job.result()
            except BaseException:
                os.remove(tmpz)
                raise
    pool.shutdown()
    # everything passed: swap the folder and the zip in
    with step("swap in folder + zip"):
        old = final + ".old"
        if os.path.exists(old):
            shutil.rmtree(old, ignore_errors=True)
        if os.path.exists(final):
            try:
                os.replace(final, old)          # a rename: fails whole (folder in use, e.g. a running host) instead of half-deleting it
            except OSError as e:
                os.remove(tmpz)
                raise SystemExit("cannot replace %s (in use? a running server / explorer window): %s. The new build stays in %s" % (final, e, out))
        os.replace(out, final)
        os.replace(tmpz, stamp_zip)
        shutil.rmtree(old, ignore_errors=True)
    print(f"{final}\n{stamp_zip}  ({size / 1e6:.0f} MB)")
    _profile(time.perf_counter() - wall0)


def _deflate9(path):
    """(raw deflate stream, crc32, size) exactly as zipfile's ZIP_DEFLATED / level 9 writer produces them"""
    comp = zlib.compressobj(9, zlib.DEFLATED, -15)
    crc = n = 0
    parts = []
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            crc = zlib.crc32(b, crc)
            n += len(b)
            parts.append(comp.compress(b))
    parts.append(comp.flush())
    return b"".join(parts), crc, n


class _Passthrough:
    def compress(self, data):
        return data

    def flush(self):
        return b""


def write_zip(tmpz, client, out, big=8 << 20):
    """The player zip: every file of `client`, deflate level 9 (Explorer's built-in extractor reads no other method). Members over 8 MB
    are compressed in worker threads at the same time (zlib releases the GIL) and written in the usual order through zipfile's own
    member writer with the finished stream - the bytes of every member equal a plain ZipFile.write (checked in tests/test_zip_parallel)."""
    paths = []
    for base, _, files in os.walk(client):
        for f in sorted(files):
            paths.append(os.path.join(base, f))
    bigs = [p for p in paths if os.path.getsize(p) > big]
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(len(bigs), 6))) as ex:
        jobs = {p: ex.submit(_deflate9, p) for p in bigs}
        with zipfile.ZipFile(tmpz, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for p in paths:
                arc = os.path.relpath(p, out).replace(os.sep, "/")
                if p not in jobs:
                    z.write(p, arc)
                    continue
                comp, crc, n = jobs[p].result()
                zi = zipfile.ZipInfo.from_file(p, arc)
                zi.compress_type = zipfile.ZIP_DEFLATED
                zi._compresslevel = 9
                with z.open(zi, "w") as dest:
                    dest._compressor = _Passthrough()          # (the stream is finished: zipfile only counts and writes it)
                    dest.write(comp)
                    dest._crc, dest._file_size = crc, n


def _ext_when_ready(ready, proc, no_cache):
    """worker: compile t2r-ext once the data build has written every generated header (build_dll creates `ready` right after
    stage_modfs: sp_maps_built.h, gunexec_*.h, vox_*.h, faction_guns_built.h ... are final then); None = the build never got there"""
    while not os.path.exists(ready):
        if proc.poll() is not None and not os.path.exists(ready):
            return None
        time.sleep(0.2)
    return build_ext(no_cache)


def prepare_host(out, host, sp_env):
    """the host folder (daemon + the modules it imports) and its checks; independent of the data build"""
    with step("host folder: copy + import checks", bg=True):
        missing = [f for f in OPTIONAL_HOST_FILES if not os.path.exists(os.path.join(ROOT, "tools", f))]
        for f in HOST_FILES:
            if f not in missing:
                shutil.copy(os.path.join(ROOT, "tools", f), host)
        # clan_story_solo.py was missing from HOST_FILES, the host's "import clan_story" failed silently and the clan feed was off.
        # Every optional import of the host files must resolve inside the host folder: import them there in a clean interpreter.
        chk = subprocess.run([sys.executable, "-I", "-c", "import sys; sys.path.insert(0, sys.argv[1]); " +
                              "; ".join("import " + m[:-3] for m in HOST_FILES if m.endswith(".py") and m != "daemon.py" and m not in missing), host],
                             capture_output=True, text=True)
        if chk.returncode:
            sys.exit("host folder is missing a module the host files import (add it to HOST_FILES): " + chk.stderr[-800:])
        for f in ("T2R-Host.ps1", "Start Server.bat"):
            shutil.copy(os.path.join(ROOT, "release", "host", f), host)
        for f in ("README.txt", "PLAYIT-GUIDE.txt", "CHANGES.txt"):
            shutil.copy(os.path.join(ROOT, "release", f), out)
        open(os.path.join(host, "sp-maps.txt"), "w").write((sp_env or "none") + "\n")    # T2R-Host.ps1 sets T2R_SP_MAPS from it
        with open(os.path.join(out, "CHANGES.txt"), "a", encoding="utf-8") as f:
            f.write("\nBuild: single-player-section maps in this client: %s (server: T2R_SP_MAPS must list them to offer them; the server only "
                    "offers a map every player of the match reports having)\n" % (sp_env or "none"))
        check_host(host)


def check_host(host):
    """The host folder must start the daemon: import it in a subprocess (scratch DB) - a missing module fails the build, not the host."""
    with tempfile.TemporaryDirectory() as td:
        code = "import sys; sys.path.insert(0, %r); import daemon" % host
        r = subprocess.run([sys.executable, "-I", "-c", code], capture_output=True, text=True, cwd=td,
                           env=dict(os.environ, T2R_DB=os.path.join(td, "check.db"), PYTHONDONTWRITEBYTECODE="1"))
    # the import creates / rotates daemon.log next to daemon.py and may leave bytecode: the shipped host folder must not carry them
    for f in os.listdir(host):
        p = os.path.join(host, f)
        if f.startswith("daemon.log") and os.path.isfile(p):
            os.remove(p)
        elif f == "__pycache__" and os.path.isdir(p):
            shutil.rmtree(p, ignore_errors=True)
    if r.returncode:
        raise SystemExit("the host folder cannot import daemon.py:\n" + r.stderr[-800:])


if __name__ == "__main__":
    main()
