"""Stock maps in the Scene Editor: Forest, Gas Station, School, Shipyard, Resort, Seraphite Camp, Houses.

Unlike the SP-section maps (data/<key>-spawns.json + sp_maps.json -> `sp_maps.py pak`), these maps are PAK-DRIVEN: their spots live in the
level pak tools/features/levels/to1-pvp-<pak>-ingame.pak (hand placed, committed; build_dll.py copies it over the mod's pak68 copy).
So the editor pak is that pak plus the markers the pak does not carry, and the import writes the saved pak back over it:

  prepare   editor pak = level pak + satchel / item-spot markers (data/<key>-satchels.json, -itemspots.json) + the forest kits
            (forest-medkits.json; the other stock maps carry their kits IN the pak as to1-healthkit-N and keep them there)
  import    diff (same rules and messages as editor_import.py); --apply writes
              * the level pak: the saved pak minus the editor-only markers (random-*, forest kits), respawns / supply boxes renumbered gap-free,
                the 12 item positions of a moved / new supply box re-derived from the box (like make_map_pak.ps1), orphan item positions dropped
              * data/<key>-satchels.json, -itemspots.json (+ tools/ext/itemspots.h) and, for Forest, data/forest-medkits.json (+ medkits.h)
Everything else of the pak (names, props such as the School gate blockers, script-icon spawners, nav, feature db) is kept byte-for-byte by the writer.

  python tools/maps/editor_stock.py prepare <key|all> [--out DIR]
  python tools/maps/editor_stock.py import <key> [saved pak] [--apply]      (editor_apply.py <key> does the same)
"""
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import t2rtmp  # noqa: E402,F401
import editor_import as EI  # noqa: E402

LEVELS = os.path.join(os.path.dirname(HERE), "features", "levels")
# image key (daemon MAPS) -> name, level pak name, level (daemon MAPS), map-list index, pool_nav / layoutlib key, where the kits live
STOCK = {
    "forest": {"name": "Forest", "pak": "forest", "level": "to1-forest", "index": 0, "pool": "forest", "kits": "data"},
    "gas-station": {"name": "Gas Station", "pak": "gas-station", "level": "to1-gas-station", "index": 1, "pool": "gas-station", "kits": "pak"},
    "school": {"name": "School", "pak": "school", "level": "to1-school", "index": 2, "pool": "school", "kits": "pak"},
    "shipyard": {"name": "Shipyard", "pak": "shipyard", "level": "to1-shipyard", "index": 3, "pool": "shipyard", "kits": "pak"},
    "resort": {"name": "Resort", "pak": "resort", "level": "to1-resort", "index": 4, "pool": "resort", "kits": "pak"},
    "seraphite-camp": {"name": "Seraphite Camp", "pak": "seraphite-camp", "level": "to1-seraphite-camp", "index": 5, "pool": "seraphite-camp", "kits": "pak"},
    "houses": {"name": "Houses", "pak": "rescue-jesse-houses", "level": "to1-jesse-houses", "index": 6, "pool": "rescue-jesse-houses", "kits": "pak"},
}
POS_TOL, ANG_TOL = EI.POS_TOL, EI.ANG_TOL


def level_pak(key):
    return os.path.join(LEVELS, f"to1-pvp-{STOCK[key]['pak']}-ingame.pak")


def ps_edit(src, dst, ops, ref=None):
    with tempfile.TemporaryDirectory() as td:
        o = os.path.join(td, "ops.json")
        json.dump(ops, open(o, "w"))
        cmd = ["pwsh", "-NoProfile", "-File", EI.EDIT, "-In", os.path.abspath(src), "-Out", os.path.abspath(dst), "-Ops", o]
        if ref:
            cmd += ["-Ref", os.path.abspath(ref)]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def prepare(key, out, ep):
    d = os.path.join(out, key)
    os.makedirs(d, exist_ok=True)
    src = level_pak(key)
    ops, ns, nr, nk = ep.ops_for(key)
    has_kits = any((s.get("art") or "").lower() == "medkit" for s in EI.read_pak(src))
    if has_kits:
        ops["add_medkits"], nk = [], sum(1 for s in EI.read_pak(src) if (s.get("art") or "").lower() == "medkit")
    dst = os.path.join(d, key + ".pak")
    ps_edit(src, dst, ops)
    shutil.copyfile(dst, os.path.join(d, key + ".baseline.pak"))
    cl = EI.classify(EI.read_pak(dst))
    print(f"{key} ({STOCK[key]['name']}): {dst}  intro {len(cl['intro'][0])}+{len(cl['intro'][1])}, respawns {len(cl['resp'])}, boxes {len(cl['box'])}, "
          f"props {len(cl['props'])}, satchels {ns}, item-spot markers {nr}, kits {nk}, other {len(cl['unknown'])}")


# ------------------------------------------------------------------ import
def _pose(s):
    return {"pos": EI.r3(s["pos"]), "quat": EI.r6(s["quat"])}


def spawns_of(co):
    """the spawns json the SP maps keep in data/<key>-spawns.json, read from the stock level pak instead"""
    num = lambda d: [_pose(d[k]) for k in sorted(d)]
    return {"player": _pose(co["player"]), "intro0": num(co["intro"][0]), "intro1": num(co["intro"][1]),
            "respawns": num(co["resp"]), "boxes": num(co["box"])}


def run(key, pak):
    st = STOCK[key]
    orig = EI.read_pak(level_pak(key))
    co = EI.classify(orig)
    cl = EI.classify(EI.read_pak(pak))
    df = EI.Diff()
    spawns = spawns_of(co)
    EI.diff_spawns(df, cl, spawns)
    props = [{"art": p["art"], **_pose(p), **({"scale": [round(x, 4) for x in p["scale"]]} if max(abs(x - 1) for x in p.get("scale", [1, 1, 1])) > EI.SCALE_TOL else {})} for p in co["props"]]
    EI.diff_props(df, cl, props)
    plan = {"spawns": None, "props": None, "prefabs": None}
    lb_path = os.path.join(EI.DATA, f"{key}-lockboxes.json")          # hand placed interrogation safes (to1-lockbox-t<team>-N), like the SP maps
    plan["lockboxes"] = EI.diff_lockboxes(df, cl, key, json.load(open(lb_path)) if os.path.exists(lb_path) else None)
    mk_path = os.path.join(EI.DATA, f"{key}-medkits.json")
    if st["kits"] == "data":
        plan["kits"] = EI.diff_kits(df, cl, key, json.load(open(mk_path)) if os.path.exists(mk_path) else None, mk_path)
    else:                                                         # the kits are pak spawners: the saved pak carries them, only report
        plan["kits"] = None
        EI.diff_kits(df, cl, key, {"kits": [{"pos": EI.r3(k["pos"])} for k in co["kits"]], "hand_placed": True}, mk_path)
    is_path = os.path.join(EI.DATA, f"{key}-itemspots.json")
    plan["spots"] = EI.diff_item_spots(df, cl, key, json.load(open(is_path)) if os.path.exists(is_path) else None)
    st_path = os.path.join(EI.DATA, f"{key}-satchels.json")
    plan["satchels"] = EI.diff_spots(df, cl, key, json.load(open(st_path)) if os.path.exists(st_path) else None, sat=True)
    known = {s["name"] for s in co["unknown"]}
    for s in cl["unknown"]:
        if s["name"] not in known:
            df.note(f"UNRECOGNISED (kept in the level pak, not interpreted): {s['name']} type {s['type']} art '{s['art']}' at {EI.fmt(s)}")
    plan["cl"], plan["co"] = cl, co
    return df, plan


def _same(a, b):
    return math.dist(a["pos"], b["pos"]) <= 0.01 and EI.ang(a["quat"], b["quat"]) <= 0.2


RX_IT = EI.re.compile(r"^to1-supplybox-(\d+)-itempos-(\d+)$")


def pak_ops(key, plan):
    """editor_edit.ps1 ops that turn the SAVED pak into the level pak"""
    st, cl, co = STOCK[key], plan["cl"], plan["co"]
    ops = {"remove": [], "place": [], "add_clone": [], "rename": []}
    ops["remove"] += [s["name"] for s in cl["lockbox"]]          # editor-only markers: the safes live in data/<key>-lockboxes.json + ext
    ops["remove"] += [s["name"] for s in cl["random"]] + [s["name"] for s in cl["spot_m"].values()] + [x[3]["name"] for x in cl["spot_i"]]
    if st["kits"] == "data":
        ops["remove"] += [s["name"] for s in cl["kits"]]
    # supply boxes: item positions follow the box (same transform make_map_pak.ps1 applies)
    oitems = {}
    for s in co["items"]:
        m = RX_IT.match(s["name"])
        oitems.setdefault(int(m.group(1)), {})[int(m.group(2))] = s
    sitems = {}
    for s in cl["items"]:
        m = RX_IT.match(s["name"])
        sitems.setdefault(int(m.group(1)), {})[int(m.group(2))] = s
    ref_n = min(n for n in co["box"] if len(oitems.get(n, {})) == 12)

    def move_items(box_old, box_new, items):
        dq = EI.qmul(tuple(box_new["quat"]), EI.qinv(tuple(box_old["quat"])))
        out = {}
        for m, it in items.items():
            rel = [it["pos"][k] - box_old["pos"][k] for k in range(3)]
            r = EI.qrot(dq, rel)
            out[m] = {"pos": [box_new["pos"][k] + r[k] for k in range(3)], "quat": list(EI.qmul(dq, tuple(it["quat"])))}
        return out
    for n, b in sorted(cl["box"].items()):
        if n in co["box"] and n in oitems:
            want = move_items(co["box"][n], b, oitems[n])
        else:
            want = move_items(co["box"][ref_n], b, oitems[ref_n])
        have = sitems.get(n, {})
        for m, w in sorted(want.items()):
            nm = f"to1-supplybox-{n}-itempos-{m}"
            if m in have:
                if not _same(have[m], w):
                    ops["place"].append({"name": nm, **w})
            else:
                src = oitems[n if n in oitems else ref_n][m]["name"]
                ops["add_clone"].append({"from": src, "name": nm, **w})
    for n, d in sitems.items():
        if n not in cl["box"]:
            ops["remove"] += [s["name"] for s in d.values()]
    # gap-free numbering (respawns, boxes + their item positions), ascending so a name is free when it is taken
    for i, n in enumerate(sorted(cl["resp"]), 1):
        if n != i:
            ops["rename"].append({"from": f"to1-spawn-point-{n}", "to": f"to1-spawn-point-{i}"})
    for i, n in enumerate(sorted(cl["box"]), 1):
        if n != i:
            ops["rename"].append({"from": f"to1-supplybox-{n}", "to": f"to1-supplybox-{i}"})
            for m in sorted(oitems[n if n in oitems else ref_n]):
                ops["rename"].append({"from": f"to1-supplybox-{n}-itempos-{m}", "to": f"to1-supplybox-{i}-itempos-{m}"})
    return ops


def apply(key, df, plan, pak, dest=None):
    dest = dest or level_pak(key)
    cats = ("player", "intro0", "intro1", "respawns", "boxes", "props", "medkits")
    if any(df.lines.get(c) for c in cats):
        ops = pak_ops(key, plan)
        tmp = dest + ".new"
        ps_edit(pak, tmp, ops, ref=level_pak(key))
        os.replace(tmp, dest)
        print(f"wrote {os.path.relpath(dest, os.path.dirname(LEVELS))}: removed {len(ops['remove'])}, placed {len(ops['place'])}, cloned {len(ops['add_clone'])}, renamed {len(ops['rename'])}")
    # kits (Forest) / item spots / satchels -> data + ext headers, same writers as the SP maps
    sub = EI.Diff()
    for c in ("medkits", "itemspots", "satchels", "lockboxes"):
        if df.lines.get(c):
            sub.lines[c] = df.lines[c]
            sub.n += len(df.lines[c])
    EI.apply(key, sub, {"spawns": None, "props": None, "prefabs": None, "kits": plan["kits"] if STOCK[key]["kits"] == "data" else None,
                        "spots": plan["spots"], "satchels": plan["satchels"], "lockboxes": plan["lockboxes"]}, {})


def main(a):
    if len(a) < 2 or a[0] not in ("prepare", "import"):
        print(__doc__)
        return 1
    import editor_prepare as ep
    if a[0] == "prepare":
        out = a[a.index("--out") + 1] if "--out" in a else ep.OUT
        keys = [x for x in a[1:] if not x.startswith("--") and x != out]
        for k in (list(STOCK) if keys == ["all"] else keys):
            prepare(k, out, ep)
        return 0
    key = a[1]
    pak = next((x for x in a[2:] if not x.startswith("--")), os.path.join(ep.OUT, key, key + ".pak"))
    df, plan = run(key, pak)
    df.show()
    if "--apply" in a and df.n:
        apply(key, df, plan, pak)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
