"""gui2-widgets.pak edits through the gui2 CLI (tools/local/gui2/gui2_cli/gui2.exe: extract to
*.widget.json, build back; round-trips byte-identical).

Weapon pickers (to1/picker-primary, to1/picker-secondary) get the skill picker's DETAILS panel
(anim-group.detailPanel: header, perkIcon, perkName, perkCost, perkDesc), same place and look, filled by
t2r-ext (feature_supp_picker): the focused gun, its level (standard / suppressed, 2013 style) and
stat bars."""
import copy
import json
import os
import shutil
import subprocess
import tempfile
import sys as _t2s, os as _t2o; _t2s.path.append(_t2o.path.normpath(_t2o.path.join(_t2o.path.dirname(_t2o.path.abspath(__file__)), ".."))); import t2rtmp  # noqa: E402,F401  (temp hygiene: tools/t2rtmp.py)
import t2guid



TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GUI2 = os.path.join(TOOLS, "local", "gui2", "gui2_cli", "gui2.exe")
SIDBASE = os.path.join(TOOLS, "win", "ext", "dcmain", "build", "sidbase.bin")


def _subtree(items, root_name):
    byg = {i["guid"]: i for i in items}
    top = next(i for i in items if i["name"] == root_name)
    keep = []
    for i in items:
        p = i
        while p is not None and p is not top:
            p = byg.get(p.get("parent"))
        if p is top:
            keep.append(i)
    return top, keep


def add_details_panel(wdir):
    skill = json.load(open(os.path.join(wdir, "to1", "picker-skill.widget.json"), encoding="utf-8"))
    top, sub = _subtree(skill["items"], "detailPanel")
    n = 0
    for page in ("picker-primary", "picker-secondary", "picker-purchase"):
        path = os.path.join(wdir, "to1", page + ".widget.json")
        w = json.load(open(path, encoding="utf-8"))
        if any(i["name"] == "detailPanel" for i in w["items"]):
            continue
        anim = next(i for i in w["items"] if i["name"] == "anim-group")
        remap = {i["guid"]: t2guid.guid() for i in sub}
        new = []
        for i in sub:
            c = copy.deepcopy(i)
            c["guid"] = remap[i["guid"]]
            c["parent"] = anim["guid"] if i is top else remap.get(i.get("parent"), i.get("parent"))
            new.append(c)
        panel = next(i for i in new if i["name"] == "detailPanel")
        new += layout_2013(new, panel)
        if page == "picker-purchase":
            # purchasables have no levels and no stat bars: just icon, name, LP cost and a tall description (filled by
            # t2r-ext feature_purch_details: what the item does + its in-match prices)
            for i in new:
                if i.get("parent") == panel["guid"] and (i["name"].startswith(("lvl", "stat")) or i["name"] == "detailDivider"):
                    i["visible"] = 0
                if i.get("parent") == panel["guid"] and i["name"] == "perkDesc":
                    i["rectSizeDelta.height"] = 760.0
        at = next(k for k, i in enumerate(w["items"]) if i["name"] == "lpTotal")
        w["items"][at:at] = new
        json.dump(w, open(path, "w", encoding="utf-8"), indent=4)
        n += 1
    return n


ROW_STEP = 148            # pickerList rows (Item%d) / rowDeco icon%d, cost%d: y = 148 * row (picker-primary's layout)


def add_secondary_rows(wdir, rows=8, page="picker-secondary"):
    """to1/picker-secondary has only 4 row widgets (Item0..3 + icon0..3 / cost0..3): the game had 4 sidearms. With the
    Enforcer that is full, so the 2013 Shorty (5th small firearm, t2r-ext install_weapon_tables) had NO row and did not
    show in GEAR UP. Rows 4..rows-1 are added the way picker-primary has its 8 (copies of row 3, 148 px
    apart, list + deco groups 8 rows tall); winmm fills / hides rows by the list count, as on the primary page (8
    widgets, the game's own count was 4). Returns the rows added."""
    path = os.path.join(wdir, "to1", page + ".widget.json")
    w = json.load(open(path, encoding="utf-8"))
    items = w["items"]
    have = {i["name"] for i in items}
    added = 0
    for r in range(4, rows):
        if f"Item{r}" in have:
            continue
        for kind, dy in (("Item", 0), ("icon", 20), ("cost", 0)):
            src = next(i for i in items if i["name"] == f"{kind}3")
            c = copy.deepcopy(src)
            c["guid"] = t2guid.guid()
            c["name"] = f"{kind}{r}"
            c["pivotAs.y"] = float(ROW_STEP * r + dy)
            if kind == "Item":
                c["text"] = ""
            at = next(k for k, i in enumerate(items) if i["name"] == f"{kind}{r - 1}") + 1
            items.insert(at, c)
        added += 1
    for name in ("pickerList", "rowDeco"):
        g = next(i for i in items if i["name"] == name)
        g["rectSizeDelta.height"] = max(g["rectSizeDelta.height"], float(ROW_STEP * (rows - 1) + 120))
    json.dump(w, open(path, "w", encoding="utf-8"), indent=4)
    return added


SCROLL_SEGS = 24          # scroll indicator thumb resolution (t2r-ext scrollbar_set lights the segments covering the visible part)
SCROLL_X = 1700.0         # right of the 1400-wide row lists (the DETAILS panel starts at x ~2500)


def add_scroll_tracks(wdir):
    """Scroll indicator on the three scrolling lists: a thin vertical separator-style track line (same polyline
    as the panel dividers, low alpha) with SCROLL_SEGS stacked bright segments on it; t2r-ext lights the ones covering the
    visible part of the list (thumb length = visible/total, position = first visible row/total) and shows the group
    "scrollBar" only when the list has more rows than fit. There is no runtime size native, hence the stacked segments."""
    n = 0
    for page, y0, hgt in (("picker-primary", 660.0, 8 * ROW_STEP), ("picker-secondary", 660.0, 8 * ROW_STEP), ("picker-purchase", 660.0, 8 * ROW_STEP),
                          ("picker-skill", 660.0, 1120.0)):
        path = os.path.join(wdir, "to1", page + ".widget.json")
        w = json.load(open(path, encoding="utf-8"))
        items = w["items"]
        if any(i["name"] == "scrollBar" for i in items):
            continue
        anim = next(i for i in items if i["name"] == "anim-group")
        src = next(i for i in items if i["name"] == "detailDivider") if any(i["name"] == "detailDivider" for i in items) else None
        if src is None:
            skill = json.load(open(os.path.join(wdir, "to1", "picker-skill.widget.json"), encoding="utf-8"))
            src = next(i for i in skill["items"] if i["name"] == "detailDivider")
        grp = copy.deepcopy(anim)
        grp.update({"guid": t2guid.guid(), "parent": anim["guid"], "name": "scrollBar", "visible": 0, "depth": 6.0, "focusable": 0, "animation": {"keyframes": {}}})
        _at(grp, SCROLL_X, y0, 10, hgt)["visible"] = 0
        new = [grp]
        def line(name, y, h, width, alpha, depth):
            c = copy.deepcopy(src)
            c.update({"guid": t2guid.guid(), "parent": grp["guid"], "name": name, "depth": depth, "color.a": alpha,
                      "lineWidth": width, "pointsUv": [0.5, 0.0, 0.5, 1.0], "closed": 0})
            _at(c, 0, y, 10, h)
            c["anchorMinN.x"] = c["anchorMaxN.x"] = 0.0
            c["pivotN.x"] = 0.0
            new.append(c)
        line("track", 0.0, hgt, 2.0, 60, 0.0)
        for k in range(SCROLL_SEGS):
            line(f"thumb{k}", hgt * k / SCROLL_SEGS, hgt / SCROLL_SEGS + 1.0, 6.0, 240, 1.0 + k * 0.01)
        at = next(k for k, i in enumerate(items) if i["name"] == "lpTotal") if any(i["name"] == "lpTotal" for i in items) else len(items)
        items[at:at] = new
        json.dump(w, open(path, "w", encoding="utf-8"), indent=4)
        n += 1
    return n


def _at(item, x, y, w, h):
    """Place an item by its top-left corner in the details panel (1180 x 1370, y down)."""
    item.update({"anchorMinN.x": 0.0, "anchorMinN.y": 0.0, "anchorMaxN.x": 0.0, "anchorMaxN.y": 0.0,
                 "pivotN.x": 0.0, "pivotN.y": 0.0, "pivotAs.x": float(x), "pivotAs.y": float(y),
                 "rectSizeDelta.width": float(w), "rectSizeDelta.height": float(h), "visible": 1})
    return item


def layout_2013(items, panel):
    """The 2013 Factions weapon details (suppressedvariantui.png): "A/D TO SELECT LEVEL", two level
    boxes (gun / can, cost bottom right, frame on the selected one), the gun icon left of its name
    and "N Points", the description, then RATE OF FIRE / RELOAD SPEED / DAMAGE / ACCURACY bars of
    10 segments. Filled by t2r-ext (feature_supp_picker)."""
    by = {i["name"]: i for i in items if i.get("parent") == panel["guid"]}
    img, txt, line = by["perkIcon"], by["perkCost"], by["detailDivider"]
    for n in ("detailHeader", "detailHeaderLine"):
        by[n]["visible"] = 0
    _at(by["perkIcon"], 40, 265, 300, 150)
    _at(by["perkName"], 360, 265, 800, 70).update({"alignH": 0, "pointSize": 40, "tint.start": "t2-white", "tint.end": "t2-white"})
    _at(by["perkCost"], 385, 335, 770, 64).update({"alignH": 0, "pointSize": 46, "tint.start": "t2-blue", "tint.end": "t2-blue"})
    _at(by["perkDesc"], 40, 440, 1100, 170).update({"pointSize": 34})
    by["perkDesc"]["pivotAs.y"] = 440.0
    _at(by["detailDivider"], 40, 625, 1100, 10)
    new = []
    def add(src, name, x, y, w, h, **kw):
        c = copy.deepcopy(src)
        c.update({"guid": t2guid.guid(), "parent": panel["guid"], "name": name, "depth": 10.0 + len(new) * 0.01})
        _at(c, x, y, w, h).update(kw)
        new.append(c)
        return c
    add(txt, "lvlHint", 40, 30, 1100, 50, text="A/D TO SELECT", alignH=1, pointSize=34,
        **{"tint.start": "t2-white", "tint.end": "t2-white"})
    for k, x in ((0, 370), (1, 610)):
        add(img, f"lvlBg{k}", x, 95, 200, 110, path="general-textures/square-32px.png", scaleMode="Stretch",
            **{"color.a": 30})
        add(line, f"lvlFrame{k}", x, 95, 200, 110, pointsUv=[0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0], closed=1,
            lineWidth=3.0, **{"color.a": 255})
        add(img, f"lvlIcon{k}", x + 12, 110, 140, 80, path="hud/weapon-cross/empty-slot.png")
        for suf, tint in (("", "t2-blue"), ("R", "t2-red")):
            add(txt, f"lvlCost{suf}{k}", x + 120, 152, 70, 50, text="2", alignH=2, pointSize=36,
                **{"tint.start": tint, "tint.end": tint})
    add(line, "lvlLine", 40, 235, 1100, 10, **{"color.a": 60})
    labels = ("RATE OF FIRE:", "RELOAD SPEED:", "DAMAGE:", "ACCURACY:")
    for s, lab in enumerate(labels):
        y = 655 + s * 130
        add(txt, f"statLabel{s}", 130, y, 800, 44, text=lab, alignH=0, pointSize=32,
            **{"tint.start": "t2-white", "tint.end": "t2-white"})
        for k in range(10):
            for suf, a in (("E", 55), ("F", 235)):
                add(img, f"stat{suf}{s}_{k}", 130 + k * 92, y + 52, 84, 18, path="general-textures/square-32px.png",
                    scaleMode="Stretch", **{"color.a": a, "tint.start": "t2-white", "tint.end": "t2-white"})
    return new


# Tactical Shotgun crosshair: the regular circle, enlarged. The circle reticle
# (engine class Hud2::ReticleCircle) sizes its "circle" group from the gun's state; its two
# ellipses (bg = drop shadow, fg = white ring) fill that group plus rectSizeDelta (4, large 8),
# so on the tight Tac (and at rest) it collapsed to a dot. These copies give the ring a base
# diameter (ND's unused experiments/shotgun-pump-reticle has a 120 circle the same way); the
# Tac's own hud2-reticle-def points at them (dc_data_patches.patch_tac_reticle).
TAC_RETICLES = {   # new widget: (source widget, ring base size)
    "weapon-reticles/tac-circle-reticle": ("weapon-reticles/circle-reticle", 120.0),
    "weapon-reticles/large/tac-circle-reticle-large": ("weapon-reticles/large/circle-reticle-large", 150.0),
    # Double Barrel: 2013's big ring - measured in a 1080p 2013 clip (re/clips/f2013ref/measurements.md B):
    # 124 px at rest (= 248 of the 2160 widget units), 159 px right after a shot; the gun's spread adds the bloom on top
    "weapon-reticles/dbl-circle-reticle": ("weapon-reticles/circle-reticle", 248.0),
    "weapon-reticles/large/dbl-circle-reticle-large": ("weapon-reticles/large/circle-reticle-large", 300.0),
    # Frontier Rifle (re/clips/f2013ref/measurements.md A): a thin ring, 19 px outer diameter at 1080p at rest (the widget
    # adds the 4-unit stroke and the gun's spread on top: base 32 of the 2160 units), ~30 px on a shot (the gun's 0.2 deg bloom)
    "weapon-reticles/frontier-circle-reticle": ("weapon-reticles/circle-reticle", 32.0),
    "weapon-reticles/large/frontier-circle-reticle-large": ("weapon-reticles/large/circle-reticle-large", 40.0),
}


TAC_RETICLE_ROOT = 1800.0   # widget units (2160 high screen): ring base 300 + a sprint spread of ~1500


def add_tac_reticles(wdir):
    n = 0
    for new, (src, size) in TAC_RETICLES.items():
        w = json.load(open(os.path.join(wdir, *src.split("/")) + ".widget.json", encoding="utf-8"))
        remap = {i["guid"]: t2guid.guid() for i in w["items"]}
        for i in w["items"]:
            i["guid"] = remap[i["guid"]]
            if i.get("parent"):
                i["parent"] = remap.get(i["parent"], i["parent"])
            if i["name"] in ("bg", "fg") and i.get("class") == "Ellipse":
                i["rectSizeDelta.width"] = i["rectSizeDelta.height"] = size
                n += 1
            elif i["name"] == "root":
                # the root group is composited (an offscreen texture of its own rect, 250 in the originals): the ring sits at the
                # rect's edge (group = the gun's spread, ring = group + base), so once the spread grows (SPRINT) the whole ring
                # lay outside the rect and was clipped away - the Double Barrel's crosshair vanished while sprinting..
                # A rect that holds the largest spread (centre anchored, pivot 0.5: nothing moves); same for every ring copy
                i["rectSizeDelta.width"] = i["rectSizeDelta.height"] = TAC_RETICLE_ROOT
        json.dump(w, open(os.path.join(wdir, *new.split("/")) + ".widget.json", "w", encoding="utf-8"), indent=4)
    assert n == 2 * len(TAC_RETICLES), "circle reticle layout changed"
    return len(TAC_RETICLES)


# Military Sniper scope view: the hunting rifle and every other scoped gun share the vector widget
# weapon-reticles/scoped/scope-reticle (Hud2::ReticleScope: thin prongs, 3 holdover notches, centre cross; no texture).
# Own copy for the Military Sniper: thick outer posts (10 units instead of 4), the notches hidden, and a mil-dot
# ladder (12-unit squares every 50 units out to 150 on all four arms, symmetric so the pivot sign does not matter).
MIL_SCOPE = "weapon-reticles/scoped/mil-scope-reticle"
MIL_SCOPE_PNG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "milsniper-scope-reticle.png")   # optional artist PNG, 1024x1024 RGBA
MIL_SCOPE_TEX = "streaming/t2r/scope/milsniper-scope-reticle.png"     # where build_dll stages it (gtex1/ + this)


def add_mil_scope(wdir):
    w = json.load(open(os.path.join(wdir, "weapon-reticles", "scoped", "scope-reticle.widget.json"), encoding="utf-8"))
    remap = {i["guid"]: t2guid.guid() for i in w["items"]}
    for i in w["items"]:
        i["guid"] = remap[i["guid"]]
        if i.get("parent"):
            i["parent"] = remap[i["parent"]]
    by = {i["name"]: i for i in w["items"]}
    for i in w["items"]:
        if i["name"].startswith("prontOuter"):
            i["rectSizeDelta.height"] = 10.0
        elif i["name"].startswith("notch"):
            i["visible"] = 0
    tmpl = by["notch1"]
    if os.path.isfile(MIL_SCOPE_PNG):        # the artist's reticle: one full-size Image child replaces the whole vector layout
        for i in w["items"]:
            if i["name"] != "root":
                i["visible"] = 0
        img = copy.deepcopy(tmpl)
        img.update({"guid": t2guid.guid(), "name": "milImage", "visible": 1, "path": MIL_SCOPE_TEX, "scaleMode": "Stretch",
                    "rectSizeDelta.width": 1024.0, "rectSizeDelta.height": 1024.0, "pivotAs.x": 0.0, "pivotAs.y": 0.0,
                    "parent": by["root"]["guid"]})
        w["items"].append(img)
        json.dump(w, open(os.path.join(wdir, *MIL_SCOPE.split("/")) + ".widget.json", "w", encoding="utf-8"), indent=4)
        return 1
    for k, d in enumerate((50.0, 100.0, 150.0)):
        for axis, sgn in (("x", 1), ("x", -1), ("y", 1), ("y", -1)):
            dot = copy.deepcopy(tmpl)
            dot.update({"guid": t2guid.guid(), "name": f"milDot{k}{axis}{'p' if sgn > 0 else 'n'}", "visible": 1,
                        "rectSizeDelta.width": 12.0, "rectSizeDelta.height": 12.0,
                        "pivotAs.x": sgn * d if axis == "x" else 0.0, "pivotAs.y": sgn * d if axis == "y" else 0.0})
            w["items"].append(dot)
    out = os.path.join(wdir, *MIL_SCOPE.split("/")) + ".widget.json"
    json.dump(w, open(out, "w", encoding="utf-8"), indent=4)
    return 1


# Revive icon (hud-buddy-status/buddy-health-attachable, shown by t2r-ext over a downed teammate): an
# Gui2::AttachableWidget projects its world anchor with a clamp-type tag read from the widget root
# ("clampType", init exe 0x142054480 region; sids none/default/flush/flush-wider/oval-flush/oval-flush-wider/
# rounded-rect/rounded-rect-flush -> -1/0..6). Without it (-1) the projection is NOT clamped (exe 0x1413af140) and the
# icon leaves the screen; with a type it goes through the clamping projection (0x1413af490 -> 0x1413ab610) and stays
# at the screen border, the offscreen arrow child (arrowEnabled) pointing at the teammate.
BUDDY_CLAMP = "default"


def clamp_buddy_icon(wdir):
    path = os.path.join(wdir, "hud-buddy-status", "buddy-health-attachable.widget.json")
    w = json.load(open(path, encoding="utf-8"))
    root = next(i for i in w["items"] if i["name"] == "root")
    assert root["engineClass"] == "Gui2::AttachableWidget" and root["tags"].get("arrowEnabled") == "true"
    root["tags"]["clampType"] = BUDDY_CLAMP
    # Bigger so players think to revive: the + and its bleed-out ring at 1.6x (80 px box -> 128 px)
    root["scale.x"] = root["scale.y"] = 1.6
    json.dump(w, open(path, "w", encoding="utf-8"), indent=4)
    return 1


def add_red_mark_widget(wdir):
    """hud-buddy-status/buddy-listen-mode-mark-red: twin of the SP listen-mode triangle (child image buddyMark is authored white,
    tint t2-white, color.a 100, inheritTint 0 - nothing set on the root recolours it) with the triangle tinted t2-red at full alpha.
    t2r-ext feature_mark_style shows it on marked enemies; teammates keep the stock widget."""
    d = os.path.join(wdir, "hud-buddy-status")
    w = json.load(open(os.path.join(d, "buddy-listen-mode-mark.widget.json"), encoding="utf-8"))
    remap = {i["guid"]: t2guid.guid() for i in w["items"]}
    for i in w["items"]:
        i["guid"] = remap[i["guid"]]
        if i.get("parent"):
            i["parent"] = remap[i["parent"]]
        if i["name"] == "buddyMark":
            i["tint.start"] = "t2-red"
            i["tint.end"] = "t2-red"
            i["color.a"] = 255
    json.dump(w, open(os.path.join(d, "buddy-listen-mode-mark-red.widget.json"), "w", encoding="utf-8"), indent=4)
    return 1


SKILL_ROWS = 32          # 2013 Factions: 13 basic + 13 DLC survival skills + 5 melee skills (t2r-ext skills.h SK_PICKER_ROWS)


def add_skill_ui(wdir):
    """2013 survival skills (t2r-ext skills.c):
    - to1/picker-skill: rows Skill10..Skill25 in the scroll list (all 26 skills), and level boxes
      (I / II / III with their LP, "A/D TO SELECT LEVEL") in the DETAILS panel, hidden by default
      (the GEAR UP loadout list uses the same page and never shows them);
    - to1/loadout-menu: a 4th skill row (SkillFour, 2013 had 4 skills), rows packed closer so the
      panel still fits."""
    n = 0
    path = os.path.join(wdir, "to1", "picker-skill.widget.json")
    w = json.load(open(path, encoding="utf-8"))
    names = {i["name"] for i in w["items"]}
    if "Skill10" not in names:
        rows = [i for i in w["items"] if i["name"].startswith("Skill") and i["name"][5:].isdigit()]
        last = max(rows, key=lambda i: int(i["name"][5:]))
        step = 230.0
        at = w["items"].index(last) + 1
        new = []
        for k in range(int(last["name"][5:]) + 1, SKILL_ROWS):
            c = copy.deepcopy(last)
            c.update({"guid": t2guid.guid(), "name": f"Skill{k}", "text": f"SKILL {k + 1}",
                      "pivotAs.y": float(k * step)})
            new.append(c)
        w["items"][at:at] = new
        contents = next(i for i in w["items"] if i["name"] == "contents")
        contents["rectSizeDelta.height"] = float(SKILL_ROWS * step - 30.0)
        panel = next(i for i in w["items"] if i["name"] == "detailPanel")
        by = {i["name"]: i for i in w["items"] if i.get("parent") == panel["guid"]}
        by["perkDesc"]["rectSizeDelta.height"] = 360.0          # 740..1100, level boxes below
        img, txt, line = by["perkIcon"], by["perkCost"], by["detailDivider"]
        lv = []

        def add(src, name, x, y, ww, h, **kw):
            c = copy.deepcopy(src)
            c.update({"guid": t2guid.guid(), "parent": panel["guid"], "name": name, "depth": 10.0 + len(lv) * 0.01})
            _at(c, x, y, ww, h).update(kw)
            c["visible"] = 0
            lv.append(c)
            return c
        add(txt, "skLvlHint", 40, 1120, 1100, 50, text="A/D TO SELECT", alignH=1, pointSize=34,
            **{"tint.start": "t2-white", "tint.end": "t2-white"})
        for k, x in enumerate((220, 480, 740)):
            add(img, f"skLvlBg{k}", x, 1185, 220, 120, path="general-textures/square-32px.png", scaleMode="Stretch",
                **{"color.a": 30})
            add(line, f"skLvlFrame{k}", x, 1185, 220, 120, pointsUv=[0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0], closed=1,
                lineWidth=3.0, **{"color.a": 255})
            add(txt, f"skLvlNum{k}", x, 1185, 220, 120, text="I" * (k + 1), alignH=1, alignV=1, pointSize=52,
                **{"tint.start": "t2-white", "tint.end": "t2-white"})
            for suf, tint in (("", "t2-blue"), ("R", "t2-red")):
                add(txt, f"skLvlCost{suf}{k}", x + 110, 1255, 100, 46, text="1", alignH=2, pointSize=34,
                    **{"tint.start": tint, "tint.end": tint})
        at = next(k for k, i in enumerate(w["items"]) if i["name"] == "lpTotal")
        w["items"][at:at] = lv
        json.dump(w, open(path, "w", encoding="utf-8"), indent=4)
        n += 1
    path = os.path.join(wdir, "to1", "loadout-menu.widget.json")
    w = json.load(open(path, encoding="utf-8"))
    if not any(i["name"] == "SkillFour" for i in w["items"]):
        three = next(i for i in w["items"] if i["name"] == "SkillThree")
        four = copy.deepcopy(three)
        four.update({"guid": t2guid.guid(), "name": "SkillFour"})
        w["items"].insert(w["items"].index(three) + 1, four)
        order = ("PrimaryWeapon", "SecondaryWeapon", "SkillOne", "SkillTwo", "SkillThree", "SkillFour", "Purchasable1")
        by = {i["name"]: i for i in w["items"]}
        spacing, h = 20.0, 160.0
        for k, nm in enumerate(order):
            by[nm]["pivotAs.y"] = k * (h + spacing)
        by["slotList"]["flowSpacing"] = spacing
        by["slotList"]["rectSizeDelta.height"] = len(order) * h + (len(order) - 1) * spacing   # 1240
        by["slotPanel"]["rectSizeDelta.height"] = by["slotPanel"]["rectSizeDelta.height"] + 80.0   # 1450
        json.dump(w, open(path, "w", encoding="utf-8"), indent=4)
        n += 1
    return n


def patch_widgets(pak_path):
    tmp = tempfile.mkdtemp(prefix="t2r_w_")
    try:
        subprocess.run([GUI2, "extract", pak_path, tmp, "--sidbase", SIDBASE], check=True, capture_output=True)
        n = add_details_panel(tmp)
        print(f"gui2-widgets: SELECT SECONDARY +{add_secondary_rows(tmp)} rows (Shorty = 5th sidearm had no row)")
        # the primary list had 8 rows; 9 base primaries + the virtual NO PRIMARY row = 10 row widgets, of which t2r-ext
        # feature_supp_picker shows a fixed window of 8 (PICK_WIN) and scrolls (list group pivot + row alpha)
        print(f"gui2-widgets: SELECT PRIMARY +{add_secondary_rows(tmp, rows=10, page='picker-primary')} rows (9 base primaries)")
        # the purchasable list had 8 rows (the page's widget count was the cap on purchasables): 12 row widgets, t2r-ext
        # feature_purch_details shows the same 8-row window and scrolls (PURCH_MAX there)
        print(f"gui2-widgets: SELECT PURCHASABLE +{add_secondary_rows(tmp, rows=12, page='picker-purchase')} rows (up to 12 purchasables)")
        print(f"gui2-widgets: scroll indicator on {add_scroll_tracks(tmp)} scrolling lists")
        add_tac_reticles(tmp)
        add_mil_scope(tmp)
        print(f"gui2-widgets: revive icon clamps to the screen edge, 1.6x ({clamp_buddy_icon(tmp)} widget, clampType={BUDDY_CLAMP})")
        print(f"gui2-widgets: red mark triangle twin buddy-listen-mode-mark-red ({add_red_mark_widget(tmp)} widget)")
        print(f"gui2-widgets: survival skills UI on {add_skill_ui(tmp)} pages (32 skill rows, level boxes, 4th skill row)")
        import clan_widgets
        panels, pages = clan_widgets.add_clan_ui(tmp)
        print(f"gui2-widgets: clan UI - main menu survivors panel ({panels}), pages to1/clan-side + to1/clan-postmatch + to1/clan-event ({pages})")
        import notice_widgets
        print(f"gui2-widgets: launch notice page to1/launch-notice ({notice_widgets.add_notice_page(tmp)})")
        print(f"gui2-widgets: kick notice page to1/kick-notice ({notice_widgets.add_kick_page(tmp)})")
        import matchflow_widgets
        print(f"gui2-widgets: match flow page to1/matchflow ({matchflow_widgets.add_matchflow_page(tmp)}), "
              f"MATCH FOUND map preview hidden ({matchflow_widgets.hide_lobby_map(tmp)} widgets), "
              f"status label twin ({matchflow_widgets.twin_status_label(tmp)}), "
              f"score banner twins ({matchflow_widgets.twin_score_banner(tmp)} widgets), "
              f"2013 match HUD: top bar cut, timer over the radar, lives under the figures ({matchflow_widgets.remake_score_banner(tmp)} widgets), "
              f"MP menu trimmed ({matchflow_widgets.trim_main_menu(tmp)} entries hidden), "
              f"disclaimer + version label hidden ({matchflow_widgets.hide_notice_and_version(tmp)} widgets), "
              f"TWELVE WEEKS names ({matchflow_widgets.rename_factions2(tmp)} texts),"
              f"find-match CREDITS hidden ({matchflow_widgets.hide_findmatch_credits(tmp)}), "
              f"2013 LOADING in the engine's loading.widget ({matchflow_widgets.add_loading_screen(tmp)} widgets)")
        import gearup13_widgets
        print(f"gui2-widgets: 2013 GEAR UP page to1/gearup13 ({gearup13_widgets.add_gearup13_page(tmp)})")
        import clan13_widgets
        print(f"gui2-widgets: in-game CLAN page to1/clan13 - roster / journal / raids ({clan13_widgets.add_clan13_page(tmp)})")
        import friends13_widgets
        print(f"gui2-widgets: in-game FRIENDS page to1/friends13 ({friends13_widgets.add_friends13_page(tmp)}), FRIENDS button under the lobby box on {friends13_widgets.add_friends_button(tmp)} pages")
        print(f"gui2-widgets: friend / party toast (frToast) on {friends13_widgets.add_friends_toast(tmp)} pages")
        import load_indicator_widgets
        print(f"gui2-widgets: TLOU2 loading icon in loading.widget ({load_indicator_widgets.add_loading_widget_spinner(tmp)} widgets)")
        import names_widgets
        print(f"gui2-widgets: change name page to1/name-entry ({names_widgets.add_names_page(tmp)})")
        import mission_widgets
        print(f"gui2-widgets: choose mission page to1/mission-choose ({mission_widgets.add_mission_page(tmp)})")
        if os.environ.get("T2R_MODEPICK_UI", "1") != "0":
          import modepick_widgets
          print(f"gui2-widgets: FIND MATCH mode box on the MP menu (native buttons {modepick_widgets.add_modepick_ui(tmp)}, arrow + open box right of the row) + CLAN menu row")
        if os.environ.get("T2R_INT_HUD_UI", "1") != "0":
          import interrogation_hud_widgets
          print(f"gui2-widgets: Interrogation HUD in hud-root (t2rIntHud bars + padlock + stage pips, t2rIntMk0/1 ATTACK / DEFEND markers, t2rIntPr hold prompt): {interrogation_hud_widgets.add_interrogation_hud(tmp)} widgets")
        import shade_widgets
        print(f"gui2-widgets: menu shade on {shade_widgets.add_shades(tmp)} sub-menu pages / details panels, name fit on {shade_widgets.fit_row_text(tmp)} row widgets")
        print(f"gui2-widgets: title screen gradient behind the buttons ({shade_widgets.add_title_grad(tmp)} widget)")
        import teamtab_widgets
        print(f"gui2-widgets: team / ready tabs (blue / red / grey / green) on {teamtab_widgets.add_team_tabs(tmp)} lobby row widgets (the team symbol icon is gone)")
        print(f"gui2-widgets: party marks (amber bar + PARTY LEAD / READY tag) on {teamtab_widgets.add_party_marks(tmp)} lobby row widgets")
        import sbteam_widgets
        print(f"gui2-widgets: scoreboards - bands follow the place (blue upper / red lower; t2r-ext sbteam.c swaps the blocks): {sbteam_widgets.add_sb_team(tmp)} pages")
        import hud_overlay_widgets
        print(f"gui2-widgets: HUD overlays in hud-root (end-of-match banner + match-start title card): {hud_overlay_widgets.add_hud_overlays(tmp)} widgets")
        import death_ui_widgets
        print(f"gui2-widgets: death screen in hud-root (black, SP death tip, KILLED BY card, countdown, spectate prompt): {death_ui_widgets.add_death_ui(tmp)} widgets")
        import killfeed_widgets
        print(f"gui2-widgets: kill feed rows in hud-root (t2rKf0-4: killer name, weapon icon, victim name; replaces the stock text feed): {killfeed_widgets.add_killfeed(tmp)} widgets")
        import radar_box_widgets
        print(f"gui2-widgets: supply box lootbox icons for the radar in hud-root (t2rRbx0-11, placed by t2r-ext radar_box.c): {radar_box_widgets.add_radar_box(tmp)} widgets")
        print(f"gui2-widgets: grey crate + arrow radar icons for supply boxes on another floor (t2rRbxU0-11 / t2rRbxD0-11, shown by t2r-ext radar_box.c): {radar_box_widgets.add_radar_box_floor(tmp)} widgets")
        print(f"gui2-widgets: radar rim arcs for supply boxes out of the radar's range (t2rRbxA0-59 white / t2rRbxG0-59 grey, shown by t2r-ext radar_box.c): {radar_box_widgets.add_radar_box_arcs(tmp)} widgets")
        import dodge_bar_widgets
        print(f"gui2-widgets: melee dodge cooldown line in hud-root (t2rDodgeBar, scaled by t2r-ext melee_dodge.c): {dodge_bar_widgets.add_dodge_bar(tmp)} widgets")
        import mate_hp_widgets
        print(f"gui2-widgets: teammate health bars under the nametags in hud-root (t2rMh*0-7, positioned + scaled by t2r-ext tag_mate_hp): {mate_hp_widgets.add_mate_hp(tmp)} widgets")
        import denynote_widgets
        print(f"gui2-widgets: blocked notice (loadout / picker refusals) on the 4 pickers + hud-root: {denynote_widgets.add_deny_notes(tmp)} widgets")
        import msg_widgets
        print(f"gui2-widgets: HUD message line in hud-root (t2rMsg / t2rMsgB, topCenterMessage copies, no text ids): {msg_widgets.add_msg_line(tmp)} widgets")
        import notify_widgets
        print(f"gui2-widgets: styled HUD notification in hud-root (t2rNtL/M/R shade + t2rNtIc icon): {notify_widgets.add_notify(tmp)} widgets")
        print(f"gui2-widgets: the same notification on the menu pages (t2rNfL/M/R + Ic + Tx under anim-group): {notify_widgets.add_notify_menu(tmp)} widgets")
        import clancard_widgets
        print(f"gui2-widgets: clan perk pop in the buy hint slot (currency-hud): {clancard_widgets.add_perk_pop(tmp)} widgets; CLAN NEWS list on the pause menu (status updates): {clancard_widgets.add_pause_status(tmp)} widgets")
        import connecting_widgets
        if os.environ.get("T2R_ENTRY_LOADING", "1") != "0":   # boat menu -> TWELVE WEEKS page in the mod's own style (pending approval)
            print(f"gui2-widgets: to1/connecting (boat menu -> TWELVE WEEKS) restyled, variant {os.environ.get('T2R_ENTRY_LOADING_VARIANT', 'L')}: "
                  f"{connecting_widgets.restyle_connecting(tmp, os.environ.get('T2R_ENTRY_LOADING_VARIANT', 'L'))} widgets added")
        # UI round 2: pause = C2, gear up = merged B2 / R3. OFF by default (the earlier map vote, pause
        # screen and post match UI stay; that build was made with T2R_UI_R2=0). T2R_UI_R2=1 turns it back on. re/ui-r2-picker-pause-impl-1019.md
        r2 = os.environ.get("T2R_UI_R2", "0") != "0"
        if r2 and os.environ.get("T2R_PAUSE_RESTYLE", "1") == "1":   # in-match Esc page: top bar + two teams side by side (C2)
            import pause_restyle_widgets
            print(f"gui2-widgets: to1/pause-menu restyled, variant {os.environ.get('T2R_PAUSE_VARIANT', 'C2')}: "
                  f"{pause_restyle_widgets.restyle_pause(tmp, os.environ.get('T2R_PAUSE_VARIANT', 'C2'))} widgets added")
        if r2 and os.environ.get("T2R_LOADOUT_R3", "1") != "0":      # gear up rework: the launcher's grid (tools/features/gearup_mock.py v3)
            import loadout_r3_widgets
            print(f"gui2-widgets: gear up pages restyled (R3 launcher grid): {loadout_r3_widgets.apply(tmp)} widgets added / moved")
        elif r2 and os.environ.get("T2R_LOADOUT_R2", "1") != "0":    # gear up: slot column | picker list | details on a grit ground (B2 merged)
            import loadout_r2_widgets
            print(f"gui2-widgets: gear up pages restyled (B2 three columns): {loadout_r2_widgets.apply(tmp)} widgets added / moved")
        import rank_badge_widgets
        print(f"gui2-widgets: rank badge (symbol + weeks) before the names: {rank_badge_widgets.add_lobby_rows(tmp)} lobby row widgets, {rank_badge_widgets.add_scoreboards(tmp)} scoreboard rows (pause menu + end page)")
        print(f"gui2-widgets: lead dev name rims (8 pink copies under each lobby / scoreboard name, t2r-ext namerim.c): {rank_badge_widgets.add_name_rims(tmp)} widgets")
        import loc_widgets
        print(f"gui2-widgets: {loc_widgets.apply(tmp)} static labels moved to text ciphers (translated with the game's language, tools/loc)")
        out = pak_path + ".new"
        subprocess.run([GUI2, "build", tmp, out, "--t2r"], check=True, capture_output=True)
        os.replace(out, pak_path)
        return n
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
