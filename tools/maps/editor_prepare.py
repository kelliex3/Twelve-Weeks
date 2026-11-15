"""Ready a shipped map for hand editing in the SceneEditor: ALL current spots of a map in ONE pak.

  python tools/maps/editor_prepare.py <key|all> [--out DIR]     # default DIR = maps-edit/ in the repo root
  -> DIR/<key>/<key>.pak          open this in TLOUSceneEditor (File > Open), edit, Save (same file)
     DIR/<key>/<key>.baseline.pak untouched copy (what the import diffs against is the json data, this one is for your undo)

The pak is built from our data exactly like the game build (sp_maps.py pak: player/camera, intros, respawns, supply boxes + their
12 item positions, cover props) and gets the spots the pak build does not carry added as editor markers:
  random-satchel-N       (art `satchel`)            supply satchel spots   (data/<key>-satchels.json)
  random-<family>-N-<art> stacks (same position)    random item spots      (data/<key>-itemspots.json)
  to1-healthkit-N        (art `medkit`)             health kits            (data/<key>-medkits.json)
Import back: python tools/maps/editor_apply.py <key> [saved pak]
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE)))
import t2rtmp  # noqa: E402,F401
import editor_import as EI  # noqa: E402
import sp_maps  # noqa: E402

OUT = r".\maps-edit"
SHIPPED = ["train-station", "hospital-ellie", "wilds-houses", "capitol-shops", "into-the-city", "overpass", "p1-bookstore", "chase", "jackson-market",
           "downtown", "jackson-suburbs", "outpost"]          # SP-section maps (data/<key>-spawns.json + sp_maps.json)
STOCK = ["forest", "gas-station", "school", "shipyard", "resort", "seraphite-camp", "houses"]   # stock maps (pak-driven, editor_stock.py)
ALL = SHIPPED + STOCK


def load(name):
    p = os.path.join(EI.DATA, name)
    return json.load(open(p)) if os.path.exists(p) else {}


def ops_for(key):
    dk = EI.DATA_KEY.get(key, key)
    random, n = [], {}
    sats = load(f"{dk}-satchels.json").get("spots", [])
    for i, s in enumerate(sats):
        random.append({"name": f"random-satchel-{i + 1}", "art": "satchel", "pos": s["pos"], "quat": s["quat"]})
    import itemspots as IS
    for i, s0 in enumerate(load(f"{dk}-itemspots.json").get("spots", [])):
        s = IS.norm_spot(s0)       # old family/arts form -> items at offset 0
        master = (f"loot-spot-{i + 1}" + ("-shared" if s.get("shared") else "")) if s.get("group") == "loot" else f"random-spot-{i + 1}"   # loot group (SceneEditor build-next25)
        random.append({"name": master, "art": "", "pos": s["pos"], "quat": s["quat"]})
        for it in s["items"]:
            if not it["item"]:
                continue
            off = EI.qrot(tuple(s["quat"]), it["offset"])
            q = EI.qmul(tuple(s["quat"]), tuple(it["quat"]))
            random.append({"name": f"random-spot-{i + 1}-i-{it['item']}" + (f"-w{it['weight']}" if it["weight"] != 1 else ""), "art": it["art"],
                           "pos": [s["pos"][k] + off[k] for k in range(3)], "quat": list(q)})
    import math
    for t, team in enumerate(load(f"{dk}-lockboxes.json").get("teams", [])):
        for k, lb in enumerate(team):
            h = math.radians(lb.get("yaw_deg", 0.0)) / 2
            random.append({"name": f"to1-lockbox-t{t}-{k + 1}", "art": "rl-cache-crate-military", "pos": lb["pos"], "quat": [0.0, math.sin(h), 0.0, math.cos(h)]})
    kits = [{"name": f"to1-healthkit-{i + 1}", "pos": k["pos"], "quat": [0.0, 0.0, 0.0, 1.0]}
            for i, k in enumerate(load(f"{dk}-medkits.json").get("kits", []))]
    return {"add_random": random, "add_medkits": kits}, len(sats), len(random) - len(sats), len(kits)


def prepare(key, out):
    if key in STOCK:
        import editor_stock
        return editor_stock.prepare(key, out, sys.modules[__name__])
    d = os.path.join(out, key)
    os.makedirs(d, exist_ok=True)
    with tempfile.TemporaryDirectory() as td:
        base = os.path.join(td, key + ".pak")
        sp_maps.pak(key, base)
        ops, ns, nr, nk = ops_for(key)
        o = os.path.join(td, "ops.json")
        json.dump(ops, open(o, "w"))
        dst = os.path.join(d, key + ".pak")
        subprocess.run(["pwsh", "-NoProfile", "-File", EI.EDIT, "-In", base, "-Out", dst, "-Ops", o], check=True, stdout=subprocess.DEVNULL)
    shutil.copyfile(dst, os.path.join(d, key + ".baseline.pak"))
    cl = EI.classify(EI.read_pak(dst))
    print(f"{key}: {dst}  intro {len(cl['intro'][0])}+{len(cl['intro'][1])}, respawns {len(cl['resp'])}, boxes {len(cl['box'])}, "
          f"props {len(cl['props'])}, satchels {ns}, item-spot markers {nr}, kits {nk}, unrecognised {len(cl['unknown'])}")


if __name__ == "__main__":
    a = sys.argv[1:]
    out = a[a.index("--out") + 1] if "--out" in a else OUT
    keys = [x for x in a if not x.startswith("--") and x != out]
    for k in (ALL if keys == ["all"] else keys):
        prepare(k, out)
