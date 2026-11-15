"""Extract the SP level paks that hold a shipped map's geometry from the game's DSAR archives (read only)
into maps-edit/<key>/geometry/, so TLOUSceneEditor can show the map behind the spot markers.

  python tools/maps/editor_geometry.py <key>|all

In the editor: File > Import Package each geometry/*.pak first (map geometry), then the marker pak <key>.pak.
Skips const / sky / audio / texture / fx / part- levels (no level geometry). Index cached in maps-edit/_paks_index.json.
"""
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)
from dsar_list import listing, open_files, extract  # noqa: E402

ROOT = os.path.dirname(TOOLS)
OUT = os.environ.get("T2R_MAPS_EDIT", "./maps-edit")
MAIN = os.environ.get("T2R_GAME_MAIN", r"C:\Games\The Last of Us Part II Remastered\build\pc\main")
KEYS = ["train-station", "hospital-ellie", "wilds-houses", "capitol-shops", "into-the-city", "overpass", "p1-bookstore", "chase", "jackson-market",
        "downtown", "jackson-suburbs", "outpost",
        "forest", "gas-station", "school", "shipyard", "resort", "seraphite-camp", "houses"]
STOCK = {"forest": "forest", "gas-station": "gas-station", "school": "school", "shipyard": "shipyard", "resort": "resort",
         "seraphite-camp": "seraphite-camp", "houses": "rescue-jesse-houses"}      # stock maps: image key -> pool_nav key (level list = the mod's level-sets-test.bin set)
STOCK_SKIP = ("-const", "sky-", "audio", "texture-adjust", "-fx", "part-", "to1-pvp-", "-render", "cubemaps", "-vis", "accessibility", "ibl-shared", "lightning")
CACHE = os.path.join(OUT, "_geometry-cache")       # one raw copy of every extracted level; the map folders hard-link it (same bytes on disk once)
SKIP = ("-const", "sky-", "audio", "texture-adjust", "-fx", "part-", "to1-pvp-", "t2r-", "-render", "cubemaps", "-vis", "accessibility", "library-path-wide", "library-path-low")   # The -wide / -low library-path levels are distant-LOD stand-ins that overlap the real buildings in the editor


def index():
    p = os.path.join(OUT, "_paks_index.json")
    if os.path.exists(p):
        return json.load(open(p))
    idx = {}
    for a in sorted(os.listdir(MAIN)):
        if not a.endswith(".psarc"):
            continue
        for path, size in listing(os.path.join(MAIN, a)):
            if path.endswith(".pak"):
                idx.setdefault(os.path.basename(path)[:-4], [a, path, size])
    os.makedirs(OUT, exist_ok=True)
    json.dump(idx, open(p, "w"), indent=0)
    return idx


def stock_levels(key):
    """level pak names (package names) of a stock map's level set; the raw -ingame / plain pak is looked up in the index by name"""
    sys.path.insert(0, HERE)
    import pool_nav
    pk = pool_nav.packages()
    out = []
    for x in pool_nav.levels_of(STOCK[key], pool_nav.bin_levels()):
        p = pk.get(x, x)
        if any(t in x or t in p for t in STOCK_SKIP):
            continue
        out.append(p.replace("-ingame", ""))
    return out


def levels(key):
    if key in STOCK:
        return stock_levels(key)
    f = os.environ.get("T2R_SP_MAPS_JSON", os.path.join(os.environ.get("T2R_WT", os.path.join(os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")), "..", "t2r-wt")), "mstats/tools/maps/sp_maps.json"))
    maps = json.load(open(f if os.path.exists(f) else os.path.join(TOOLS, "maps", "sp_maps.json")))
    m = maps.get("maps", maps)
    v = m[key] if isinstance(m, dict) else next(x for x in m if x.get("key") == key)
    return [lv for lv in v["levels"] if not any(s in lv for s in SKIP)]


def level_report(key):
    """every level of the map's set and why it is in / out of the editor geometry (written to geometry/_levels.json)"""
    rows = []
    if key in STOCK:
        sys.path.insert(0, HERE)
        import pool_nav
        pk = pool_nav.packages()
        for x in pool_nav.levels_of(STOCK[key], pool_nav.bin_levels()):
            p = pk.get(x, x)
            hit = next((t for t in STOCK_SKIP if t in x or t in p), None)
            rows.append({"level": p.replace("-ingame", ""), "in": hit is None, "why": "level-sets-test.bin entry of the stock map" if hit is None else f"no level geometry (name contains '{hit}')"})
        return {"source": "tools/maps/pool_nav.levels_of (level-sets-test.bin)", "levels": rows}
    f = os.environ.get("T2R_SP_MAPS_JSON", os.path.join(os.environ.get("T2R_WT", os.path.join(os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")), "..", "t2r-wt")), "mstats/tools/maps/sp_maps.json"))
    maps = json.load(open(f if os.path.exists(f) else os.path.join(TOOLS, "maps", "sp_maps.json")))
    m = maps.get("maps", maps)
    v = m[key] if isinstance(m, dict) else next(x for x in m if x.get("key") == key)
    for lv in v["levels"]:
        hit = next((t for t in SKIP if t in lv), None)
        rows.append({"level": lv, "in": hit is None, "why": "sp_maps.json 'levels' (the build's level set; our own spawner pak is not geometry)" if hit is None else f"no level geometry (name contains '{hit}')"})
    return {"source": "tools/maps/sp_maps.json levels", "levels": rows}


def apply_mp_edits(key, dst):
    """What the MP build ships is the SP level WITH our level edits: sp_maps.json "edits", the workbench-strip edits of the build
    (collectibles.bench_edits, sp_maps_build.stage_edits adds them to every map) and the Scene Editor's geoedits (tools/maps/geoedit.py).
    The raw SP paks just extracted are replaced by the edited render paks, so the editor shows exactly the geometry of the MP map.
    Writes <dst>/_mp-edits.json: every applied edit with its source (the editor lists them read-only in the Removed geometry panel).
    (The collectible strip of the build only parks spawner entities, which are not drawn in the editor.)"""
    sys.path.insert(0, HERE)
    import level_edit
    import geoedit
    rows = []
    edits = []
    try:
        spec_edits = level_edit.spec_edits(key)
    except Exception:                                   # a stock map has no sp_maps.json entry (no spec edits)
        spec_edits = []
    for e in spec_edits:
        edits.append(e)
        rows.append({"name": e.get("name"), "level": e["level"], "action": e["action"], "source": "sp_maps.json edits"})
    if os.environ.get("T2R_NO_BENCH") != "1" and key not in STOCK:
        try:
            import collectibles
            be, _rep = collectibles.bench_edits(key, lambda *a, **k: None)
            for e in be:
                edits.append(e)
                rows.append({"name": e.get("name"), "level": e["level"], "action": e["action"], "source": "workbench strip (sp_maps_build.stage_edits)"})
        except Exception as ex:                       # pwsh dumps need the SceneEditor Core DLL; the geometry is still right without the benches
            print(f"{key}: workbench strip edits skipped ({ex})", flush=True)
    for e in geoedit.edits_for(key):
        if e["action"] == "colledit":                    # collision only (coll_edit.py): nothing for the render geometry
            continue
        edits.append(e)
        rows.append({"name": e.get("name"), "level": e["level"], "action": e["action"], "source": "geoedits (Scene Editor)"})
    json.dump({"key": key, "edits": rows}, open(os.path.join(dst, "_mp-edits.json"), "w"), indent=1)
    if not edits:
        print(f"{key}: no level edits (geometry = the raw SP paks)", flush=True)
        return
    out = os.path.join(level_edit.CACHE, "out", key + "-editor")
    files = level_edit.build_edits(edits, out_dir=out)
    n = 0
    for f in files:
        name = os.path.basename(f)
        if name.endswith("-phys.pak") or not os.path.exists(os.path.join(dst, name)):
            continue
        import shutil
        d2 = os.path.join(dst, name)
        if os.path.exists(d2):
            os.remove(d2)                               # never write through a hard link to the cache
        shutil.copyfile(f, d2)
        n += 1
    print(f"{key}: {len(edits)} level edit(s) applied to {n} geometry pak(s) in {dst}", flush=True)


P1_NEAR = ("hun-checkpoint-bookstore", "hun-checkpoint-bookstore-rear", "hun-checkpoint-bookstore-facade", "hun-checkpoint-maze")   # geometry/ (loaded with the map); the other Part I levels go to geometry-lod/ (File > Import Package when wanted)
P1_SKIP = ("sky-", "-rs", "-audio")                                                                                                     # no level geometry (-audio: the Part I ambience levels hun-const-audio / -audio-rgn)


def _put(src, dst):
    """hard link (copy when that fails) src -> dst unless dst already is that file (same size, not older)"""
    if os.path.exists(dst):
        if os.path.getsize(dst) == os.path.getsize(src) and os.path.getmtime(dst) >= os.path.getmtime(src):
            return False
        os.remove(dst)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copyfile(src, dst)
    return True


def p1_geometry(key, dst):
    """Part I map (p1-bookstore): the converted level paks + their -phys.pak collision from the converted folder (p1_src.conv_dir(), $T2R_P1_CONV),
    not from the game archives.  The near levels (bookstore, rear, facade, maze) go to <dst>, the rest of the map's level set to <dst>-lod (not loaded
    by the editor unless imported).  The -phys.pak must NOT sit next to the level pak (the editor imports every *.pak of the geometry folder as geometry): it is linked into
    the level-edit-cache/src folders instead, where CollisionCache.FindPhysPak looks (this checkout's, the main checkout's).
    Files of an older conversion (other size / older) are replaced; paks of levels no longer in the set are removed."""
    import p1_src
    conv = p1_src.conv_dir()
    if not conv:
        raise SystemExit(f"{key}: no complete converted Part I folder found (re/part1/x/conv*; python tools/part1/p1_levelset.py convert ... or set T2R_P1_CONV)")
    lod = dst + "-lod"
    os.makedirs(lod, exist_ok=True)
    want = {}
    phys = {}
    rows = []
    for lv in p1_src.levels_of(key):
        hit = next((t for t in P1_SKIP if t in lv), None)
        rows.append({"level": lv, "in": hit is None, "where": None if hit else ("geometry" if lv in P1_NEAR else "geometry-lod"),
                     "why": f"no level geometry (name contains '{hit}')" if hit else "sp_maps.json part1.levels (converted Part I level)"})
        if hit:
            continue
        d = dst if lv in P1_NEAR else lod
        s = os.path.join(conv, lv + ".pak")
        if os.path.exists(s):
            want[os.path.join(d, lv + ".pak")] = s
        sp = os.path.join(conv, lv + "-phys.pak")
        if os.path.exists(sp):
            phys[sp] = lv + "-phys.pak"
    got = []
    for f, s in want.items():
        got.append(f"{os.path.basename(f)} ({os.path.getsize(s) // 1048576} MB{'' if _put(s, f) else ', current'})")
    for d in (dst, lod):
        for n in os.listdir(d):
            f = os.path.join(d, n)
            if n.endswith(".pak") and f not in want and os.path.isfile(f):
                os.remove(f)
                got.append(f"{n}: removed (not in the level set)")
    import level_edit
    caches = []
    for c in (level_edit.CACHE, os.path.join(os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")), "re/maps/level-edit-cache"), os.path.join(os.environ.get("T2R_WT", os.path.join(os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")), "..", "t2r-wt")), "mstats/re/maps/level-edit-cache")):
        n = os.path.normcase(os.path.abspath(c))
        if os.path.isdir(os.path.dirname(c)) and n not in [os.path.normcase(os.path.abspath(x)) for x in caches]:
            caches.append(c)
    nph = 0
    for c in caches:
        os.makedirs(os.path.join(c, "src"), exist_ok=True)
        for s, n in phys.items():
            nph += bool(_put(s, os.path.join(c, "src", n)))
    got.append(f"{len(phys)} -phys.pak (collision) linked into {len(caches)} level-edit-cache/src folder(s), {nph} new")
    print(f"{key}: from {conv}: " + ", ".join(got), flush=True)
    json.dump({"source": "tools/maps/sp_maps.json part1.levels, converted folder " + conv, "levels": rows}, open(os.path.join(dst, "_levels.json"), "w"), indent=1)


def main(keys):
    idx = index()
    opened = {}
    for key in keys:
        dst = os.path.join(OUT, key, os.environ.get("T2R_GEOM_DIR", "geometry"))      # T2R_GEOM_DIR=geometry-next: regenerate beside the folder an open editor uses
        os.makedirs(dst, exist_ok=True)
        import p1_src
        if p1_src.levels_of(key):
            p1_geometry(key, dst)
            apply_mp_edits(key, dst)
            continue
        if os.environ.get("T2R_LEVELS_ONLY") == "1":          # only (re)write geometry/_levels.json
            json.dump(level_report(key), open(os.path.join(dst, "_levels.json"), "w"), indent=1)
            print(f"{key}: _levels.json", flush=True)
            continue
        got = []
        for lv in levels(key):
            for name in (lv, lv + "-ingame"):
                if name not in idx:
                    continue
                a, path, size = idx[name]
                f = os.path.join(dst, name + ".pak")
                if not (os.path.exists(f) and os.path.getsize(f) == size):
                    c = os.path.join(CACHE, name + ".pak")
                    if not (os.path.exists(c) and os.path.getsize(c) == size):
                        if a not in opened:
                            opened[a] = open_files(os.path.join(MAIN, a))
                        os.makedirs(CACHE, exist_ok=True)
                        with open(c + ".tmp", "wb") as o:
                            o.write(extract(*opened[a], path))
                        os.replace(c + ".tmp", c)
                    if os.path.exists(f):
                        os.remove(f)
                    try:
                        os.link(c, f)
                    except OSError:
                        shutil.copyfile(c, f)
                got.append(f"{name} ({size // 1048576} MB)")
                break
            else:
                got.append(f"{lv}: NOT FOUND")
        print(f"{key}: " + ", ".join(got), flush=True)
        try:
            json.dump(level_report(key), open(os.path.join(dst, "_levels.json"), "w"), indent=1)
        except Exception as ex:
            print(f"{key}: _levels.json skipped ({ex})", flush=True)
        apply_mp_edits(key, dst)


if __name__ == "__main__":
    a = sys.argv[1:] or ["all"]
    main(KEYS if a == ["all"] else a)
