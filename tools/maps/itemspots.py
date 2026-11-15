"""Random item spots python itemspots.py  ->  tools/ext/itemspots.h from data/<key>-itemspots.json

A spot is a place on a map where the ext (tools/ext/itemspots.c) puts ONE pickup out of a short candidate list - or nothing: every roll fills the
spot with ISP_FILL_PCT % chance, the item is picked uniformly from the candidates; a taken pickup is back after ISP_RESPAWN_S with a new roll.
The spots come from the SceneEditor: spawners whose name starts with `random-` (e.g. random-util-1-molotov, random-util-2-pipebomb ...) that
sit together (within 0.5 m of each other, or parented into each other) are ONE spot; the candidates are their art groups.  editor_import.py
reads them (never written into the generated pak: the ext spawns the pickups).

data/<key>-itemspots.json: {"key", "map_index", "hand_placed": true, "spots": [{"pos": [x y z], "quat": [x y z w], "family": "util",
                            "arts": ["molotov", "pipe-bomb", ...]}]}
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, HERE)
DATA = os.path.join(HERE, "data")
HEADER = os.path.join(TOOLS, "ext", "itemspots.h")
MAX_CAND = 40

# THE spawnable list (everything spawnable in supply boxes, melee included): id = the game item id box_spawn_like / the supply boxes use,
# art = the editor art group that draws it (1:1 preview), count = pickup count (ammo = the 2013 per-pickup size of supplybox.c B13_AMMO, melee = durability by the ext)
# EVERY ammo-* id rolled by a map spot is a CLASS in game (itemspots.c isp_ammo_for_me): ammo-pistol / ammo-revolver* = the pistol ammo of the player
# who sees it, any other ammo-* = ammo of one of his long guns; a player without such a gun sees an empty spot. Use ammo-pistol / ammo-rifle in new pools.
# Sources: tools/ext/supplybox.c (ingredients BOX_INGREDIENTS, B13_AMMO, health-kit / health-small, B13_GAPS throwables, MELEE_DROPS pool), t2r-ext.c MELEE_DROP_POOL.
CATS = ["Ammo", "Crafting ingredients", "Throwables and utility", "Health", "Melee: 1h blunt", "Melee: 1h blade", "Melee: 2h blunt"]
ITEMS = [
    ("ammo-pistol", 0, "Pistol ammo (tray) - in game: ammo for the pistol the player carries", "gun-ammo-tray-pistol-look", 6),
    ("ammo-revolver", 0, "Revolver ammo (tray)", "gun-ammo-tray-revolver-look", 5),
    ("ammo-revolver-joel", 0, "Revolver ammo (Joel, tray)", "gun-ammo-tray-revolver-look", 5),
    ("ammo-rifle", 0, "Rifle ammo (boxed) - in game: ammo for a long gun the player carries", "rifle-ammo-box", 4),
    ("ammo-rifle-vepr", 0, "Rifle ammo (Vepr, boxed)", "rifle-ammo-box", 5),
    ("ammo-rifle-mpx5", 0, "Rifle ammo (MPX5 / burst, boxed)", "rifle-ammo-box", 8),
    ("ammo-shotgun", 0, "Shotgun shells (boxed)", "shotgun-ammo-box", 4),
    ("ammo-shotgun-baserri", 0, "Shotgun shells (over-under, boxed)", "shotgun-ammo-box", 4),
    ("ammo-arrow", 0, "Arrows", "arrow-ammo-arrow", 3),
    ("ammo-crossbow", 0, "Crossbow bolts", "arrow-ammo-crossbow", 3),
    ("alcohol", 1, "Alcohol", "alcohol-booze", 2),
    ("rag", 1, "Rag", "rag-cloth", 2),
    ("canister", 1, "Canister", "canister-metal", 2),
    ("explosive", 1, "Explosive", "explosive-gunpowder-med", 2),
    ("binding", 1, "Binding", "binding-duct", 2),
    ("blade", 1, "Blade", "blade-simple", 2),
    ("molotov", 2, "Molotov", "molotov", 1),
    ("pipe-bomb", 2, "Pipe bomb", "pipe-bomb", 1),
    ("smoke-bomb", 2, "Smoke bomb", "stun-bomb", 1),
    ("shiv", 2, "Shiv", "knife-shiv", 1),
    ("health-kit", 3, "Health kit", "medkit", 1),
    ("health-small", 3, "Snack bar", "health-pickup-granolabar", 1),
    ("crowbar", 4, "Crowbar", "knife-crowbar-one-hand", 0),
    ("pipe-wrench", 4, "Pipe wrench", "knife-pipe-wrench", 0),
    ("hammer-modern", 4, "Hammer", "knife-hammer-modern", 0),
    ("hammer-scar", 4, "Hammer (Scar)", "knife-hammer-scar", 0),
    ("club-1h-police", 4, "Police baton", "knife-club-1h-police", 0),
    ("club-1h-scar", 4, "Club (Scar)", "knife-club-1h-scar", 0),
    ("tomahawk", 5, "Tomahawk", "knife-hatchet", 0),
    ("hatchet", 5, "Hatchet", "knife-hatchet-modern", 0),
    ("two-by-four", 6, "2x4", "two-hand-melee-two-by-four", 0),
    ("baseball-bat", 6, "Baseball bat", "two-hand-melee-baseball-bat", 0),
    ("lead-pipe", 6, "Lead pipe", "two-hand-melee-lead-pipe", 0),
]
ITEM_BY_ID = {i[0]: i for i in ITEMS}
# legacy editor art group -> item id (old random-<family>-N-<art> stacks)
ART_ITEM = {"molotov": "molotov", "pipe-bomb": "pipe-bomb", "knife-shiv": "shiv", "stun-bomb": "smoke-bomb", "medkit": "health-kit",
            "satchel": "parts"}      # satchels.py: a spot whose only candidate is `parts` = a dedicated supply satchel spot (itemspots.c ISP_SATCHEL)
ITEM_IDS = set(ITEM_BY_ID) | {"parts"}
MELEE_CAT = (4, 5, 6)


def item_of(art):
    a = (art or "").lower()
    return ART_ITEM.get(a) or (a if a in ITEM_IDS else None)


def art_of(item):
    return ITEM_BY_ID[item][3] if item in ITEM_BY_ID else item


def _r3(v):
    return [round(float(x), 3) for x in v]


def _r6(v):
    return [round(float(x), 6) for x in v]


def norm_item(it):
    """canonical item entry: {item, art, offset[3], quat[4], weight}"""
    item = it.get("item") or item_of(it.get("art")) or ""
    return {"item": item, "art": it.get("art") or art_of(item), "offset": _r3(it.get("offset") or [0, 0, 0]),
            "quat": _r6(it.get("quat") or [0, 0, 0, 1]), "weight": int(it.get("weight") or 1)}


def norm_spot(s):
    """a spot of either json form -> {pos, quat, items:[norm_item]} (old form: family + arts -> items at offset 0)"""
    if "items" in s:
        items = [norm_item(i) for i in s["items"]]
    else:
        items = [norm_item({"art": a}) for a in s.get("arts", [])]
    out = {"pos": _r3(s["pos"]), "quat": _r6(s["quat"]), "items": items}
    if s.get("family") and "items" not in s:
        out["family"] = s["family"]
    for k in SPOT_ATTRS:            # loot scatter attributes (kept through the editor round trip by editor_import.py)
        if k in s and s[k] not in (None, False, 0, ""):
            out[k] = s[k]
    return out


# per-spot attributes (tools/maps/loot_place.py): group = the placeable group ("loot" = the scatter of ingredients / ammo / health between the
# supply boxes, "" = a hand item spawner), shared = every player gets his own copy (itemspots.c sh), fill = % chance a roll fills the spot
# (0 = ISP_FILL_PCT), resp = seconds until it rolls again (0 = ISP_RESPAWN_S), dy = visible surface - collision surface (m, added to the ray hit),
# room = the room cluster the budget was computed for (info only).
SPOT_ATTRS = ("group", "room", "shared", "fill", "resp", "dy", "surface")


def ser_item(i):
    d = {"item": i["item"]}
    if i["art"] != art_of(i["item"]):
        d["art"] = i["art"]
    if any(abs(v) > 1e-6 for v in i["offset"]):
        d["offset"] = i["offset"]
    if any(abs(a - b) > 1e-6 for a, b in zip(i["quat"], (0, 0, 0, 1))):
        d["quat"] = i["quat"]
    if i["weight"] != 1:
        d["weight"] = i["weight"]
    return d


def ser_spot(s):
    d = {"pos": s["pos"], "quat": s["quat"], "items": [ser_item(i) for i in s["items"]]}
    for k in SPOT_ATTRS:
        if k in s and s[k] not in (None, False, 0, ""):
            d[k] = s[k]
    return d


def write_catalog():
    """tools/maps/data/spot_items.json: the checklist the SceneEditor reads (resources/spot-items.json)"""
    d = {"cats": CATS, "items": [{"id": i[0], "cat": i[1], "label": i[2], "art": i[3], "count": i[4]} for i in ITEMS]}
    p = os.path.join(DATA, "spot_items.json")
    json.dump(d, open(p, "w", newline=chr(10)), indent=1)
    return p


# stock maps (daemon MAPS order = ext MF_MAPS): not in medkits.INDEX (their kits live in the level pak), but their item / satchel spots are map-indexed too
STOCK_INDEX = {"forest": 0, "gas-station": 1, "school": 2, "shipyard": 3, "resort": 4, "seraphite-camp": 5, "houses": 6}
EXTRA_INDEX = {"chase": 22, "jackson-market": 23, "p1-bookstore": 17}     # SP-section maps without health-kit data (not in medkits.INDEX): editor_import.MAP_INDEX


def index_of(key):
    import medkits
    if key in medkits.INDEX:
        return medkits.INDEX[key]
    if key in EXTRA_INDEX:
        return EXTRA_INDEX[key]
    return STOCK_INDEX[key]


def build(key, spots):
    """the json dict for a list of {pos, quat, family, arts}"""
    return {"key": key, "map_index": index_of(key), "hand_placed": True,
            "rule": "item spawners of the SceneEditor (tools/maps/editor_import.py): ISP_FILL_PCT % filled, weighted pick of one item, back after ISP_RESPAWN_S",
            "spots": [ser_spot(norm_spot(s)) if "items" in s or "arts" in s else s for s in spots]}


def save(key, spots):
    p = os.path.join(DATA, f"{key}-itemspots.json")
    json.dump(build(key, spots), open(p, "w", newline=chr(10)), indent=1)
    write_header()


def write_header():
    rows = {}
    for suf in ("-itemspots.json", "-satchels.json"):         # the editor's random spots first, then satchels.py's fixed satchel spots
        for fn in sorted(os.listdir(DATA)):
            if fn.endswith(suf):
                d = json.load(open(os.path.join(DATA, fn)))
                if d.get("spots"):
                    if d["key"] in rows:
                        rows[d["key"]] = dict(rows[d["key"]], spots=rows[d["key"]]["spots"] + d["spots"])
                    else:
                        rows[d["key"]] = d
    rows = [rows[k] for k in sorted(rows)]
    L = ["// generated by tools/maps/itemspots.py - item spawners (SceneEditor `random-spot-N` + item markers; legacy random-* stacks), per map-list index (daemon MAPS / ext MF_MAPS)",
         "// y = the editor position; the ext drops the pickup on the surface under it. c[k] = a candidate: game item id, offset (spot-local, rotated by q), rotation, weight.",
         f"#define ISP_MAXC {MAX_CAND}",
         "typedef struct { const char *item; float off[3]; float q[4]; int w; } isp_cand_t;",
         "// 11-03: dy = visible surface - collision under the spot (added to the ray hit), sh = shared (per-player copy), fill = % (0 = ISP_FILL_PCT), resp = s (0 = ISP_RESPAWN_S)",
         "typedef struct { float p[3]; float q[4]; int n; isp_cand_t c[ISP_MAXC]; float dy; int sh, fill, resp; } isp_spot_t;"]
    for r in rows:
        nm = r["key"].upper().replace("-", "_")
        L.append(f"static const isp_spot_t ISP_{nm}[] = {{   // map {r['map_index']} ({r['key']}): {len(r['spots'])} spots")
        for s in r["spots"]:
            ns = norm_spot(s)
            items, seen = [], set()
            for i in ns["items"]:
                if i["item"] and (i["item"] not in seen):
                    seen.add(i["item"])
                    items.append(i)
            items = items[:MAX_CAND]
            if not items:
                continue
            cs = ", ".join('{"%s", {%.3ff, %.3ff, %.3ff}, {%.6ff, %.6ff, %.6ff, %.6ff}, %d}' % ((i["item"],) + tuple(i["offset"]) + tuple(i["quat"]) + (i["weight"],)) for i in items)
            ex = ns.get("group") == "loot" or any(ns.get(k) for k in ("dy", "shared", "fill", "resp"))
            tail = ", %.3ff, %d, %d, %d" % (float(ns.get("dy") or 0), 1 if ns.get("shared") else 0, int(ns.get("fill") or 0), int(ns.get("resp") or 0)) if ex else ""
            L.append("    {{%.3ff, %.3ff, %.3ff}, {%.6ff, %.6ff, %.6ff, %.6ff}, %d, {%s}%s}," % (
                s["pos"][0], s["pos"][1], s["pos"][2], s["quat"][0], s["quat"][1], s["quat"][2], s["quat"][3], len(items), cs, tail))
        L.append("};")
    L.append("static const struct { int map; const isp_spot_t *p; int n; } ISP_TAB[] = {")
    for r in rows:
        nm = r["key"].upper().replace("-", "_")
        L.append(f"    {{{r['map_index']}, ISP_{nm}, (int)(sizeof ISP_{nm} / sizeof *ISP_{nm})}},")
    L.append("    {-1, 0, 0},")
    L.append("};")
    open(HEADER, "w", newline=chr(10)).write(chr(10).join(L) + chr(10))
    print(f"{HEADER}: {len(rows)} map(s)")


if __name__ == "__main__":
    write_catalog()
    write_header()
