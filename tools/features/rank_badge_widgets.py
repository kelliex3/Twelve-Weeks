"""The rank badge in front of every player name - ONE geometry for every screen (the ranks show in front of the names in the lobby and on every screen,
not just in the main menu).

A badge = the chalk rank symbol (image, streaming/t2r/icons/rank-<n>.png, swapped by t2r-ext rank_ui) + the weeks survived in total (small Alternate-face text),
exactly the pair the PLAYER STATS page draws (clan13_widgets xpRk / xpRn). Its size follows the point size of the NAME it stands before:

    layout   [symbol][weeks as a superscript at the symbol's top-right][gap][name]   (rank + weeks + name)
    symbol   square box, the chalk glyph (it fills 7/8 of the art canvas, tools/models/rank_icons.py) is 75 % of the ROW height, vertically centred on the row
    weeks    0.54 pt (bolder look: bigger + brighter chalk), left aligned right after the glyph's visible edge (+ 0.10 row), centred 0.27 row above the row centre,
             widget box 1.9 x its size (three digits); the reserved slot is two digits (1.3 x), so the name column is the same for every player
    gap      0.25 row from the end of the two-digit weeks slot to the name          reserved(pt, row_h) = the width the name moves right by

Both are vertically centred on the name's box (the name text is centred in its box, alignV 1). Widget names (t2r-ext rankbadge.c / the screen drivers):
    lobby rows (lobby-row, find-match-row)   rkIcon / rkNum         scoreboard (pause-menu + scoreboard pages)   team<t>_row<r>_rkIcon / _rkNum
    map vote roster                            ros<r>Rk / ros<r>Rn   friends panel rows                          <p>Rk<i> / <p>Rn<i>
"""
import copy
import t2guid

ICON = "streaming/t2r/icons/rank-0.png"
BOX = "streaming/t2r/icons/weeks-box.png"     # thin outlined box 55:36 (tools/models/rank_icons.weeks_box), style 3
CAP_ARGB = 0xFF8C897E
ICON_ARGB = 0xFFECE8DC          # the stats page's chalk white
NUM_ARGB = 0xFFE0DCCE         # brighter than the old grey: the small digits read bolder


LOBBY_X0 = 40.0              # lobby / searching rows (1080 wide): the badge starts at the row's start inset = the stock start icons' left edge (spinner / team symbol: 60 - 44 / 2 = 38). It was 120 = the stock NAME x (already clear of that icon column), so badge + reserved put the name a quarter of the row in
GLYPH_FILL = 0.875          # the art's glyph / canvas (rank_icons.glyph fits every symbol into 112 of 128 px)
ROW_FILL = 0.75             # glyph height / row height


def metrics(pt, row_h):
    """Geometry of a badge for a name of point size `pt` standing in a row of height `row_h` (same units). Offsets are from x = the VISIBLE left edge of the chalk (the column's text start; the symbol box starts `lead` left of it)."""
    icon = int(round(row_h * ROW_FILL / GLYPH_FILL))
    num_pt = int(round(pt * 0.54))
    num_w = int(round(num_pt * 1.9))
    lead = int(round(icon * (1.0 - GLYPH_FILL) / 2.0))                       # empty canvas margin left of the chalk: the box starts this far left of x, so the VISIBLE chalk starts exactly at x (the column's text start)
    num_x = int(round(icon * GLYPH_FILL + row_h * 0.10))                    # from x (visible chalk edge): 0.10 row after the glyph's visible right edge
    num_dy = row_h * 0.27                                                    # up from the row centre
    gap = int(round(row_h * 0.25))
    slot = int(round(num_pt * 1.3))                                          # reserved width = TWO digits (the name x is static in the JSON); a 3-digit number reaches into the gap
    return {"icon": icon, "lead": lead, "num_pt": num_pt, "num_w": num_w, "num_x": num_x, "num_dy": num_dy, "gap": gap, "reserved": num_x + slot + gap}


def reserved(pt, row_h):
    return metrics(pt, row_h)["reserved"]


def add_to_builder(b, parent, icon_name, num_name, x, cy, pt, row_h, visible=0):
    """On a clan_widgets.Builder page: the badge with its left edge at x and its vertical centre at cy (box coordinates of `parent`)."""
    m = metrics(pt, row_h)
    nh = m["num_pt"] * 1.5
    b.image(icon_name, parent, b.tl(x - m["lead"], cy - m["icon"] / 2.0, m["icon"], m["icon"]), ICON, ICON_ARGB, 255, scaleMode="StretchAspect", visible=visible)
    b.text(num_name, parent, b.tl(x + m["num_x"], cy - m["num_dy"] - nh / 2.0, m["num_w"], nh), "", m["num_pt"], NUM_ARGB, align=0, face="Alternate", tracking=2, visible=visible)


def add_to_items(items, parent_guid, tmpl_img, tmpl_txt, icon_name, num_name, x, cy, pt, row_h, depth, visible=0):
    """On a native page (a list of widget dicts): clones of an existing image / text item of that page placed top-left anchored at (x, cy)."""
    m = metrics(pt, row_h)
    pos = {"anchorMinN.x": 0.0, "anchorMinN.y": 0.0, "anchorMaxN.x": 0.0, "anchorMaxN.y": 0.0, "pivotN.x": 0.0, "pivotN.y": 0.5}
    ic = copy.deepcopy(tmpl_img)
    ic.update(pos)
    ic.update({"guid": t2guid.guid(), "name": icon_name, "parent": parent_guid, "depth": depth, "visible": visible, "path": ICON, "scaleMode": "StretchAspect",
               "alignH": 1, "alignV": 1, "color.a": 255, "tint.start": ICON_ARGB, "tint.end": ICON_ARGB, "tint.blend": 0.0,
               "pivotAs.x": float(x - m["lead"]), "pivotAs.y": float(cy), "rectSizeDelta.width": float(m["icon"]), "rectSizeDelta.height": float(m["icon"])})
    ic.pop("slices", None)
    tx = copy.deepcopy(tmpl_txt)
    tx.update(pos)
    tx.update({"guid": t2guid.guid(), "name": num_name, "parent": parent_guid, "depth": depth + 0.01, "visible": visible, "text": "", "pointSize": m["num_pt"], "faceId": "Alternate",
               "alignH": 0, "alignV": 1, "color.a": 255, "tint.start": NUM_ARGB, "tint.end": NUM_ARGB, "tint.blend": 0.0, "multiline": 0, "tracking.x": 2.0,
               "pivotAs.x": float(x + m["num_x"]), "pivotAs.y": float(cy - m["num_dy"]), "rectSizeDelta.width": float(m["num_w"]), "rectSizeDelta.height": float(m["num_pt"] * 1.5)})
    tx.pop("animation", None)
    tx["animation"] = {"keyframes": {}}
    tx.pop("effect", None)
    items.extend([ic, tx])
    return ic, tx


def _load(wdir, page):
    import json
    import os
    path = os.path.join(wdir, "to1", page + ".widget.json")
    return path, json.load(open(path, encoding="utf-8"))


def _save(path, w):
    import json
    json.dump(w, open(path, "w", encoding="utf-8"), indent=4)


def add_lobby_rows(wdir):
    """lobby-row (MP menu rows + find-match Player0) and find-match-row (searching / MATCH FOUND rows), 1080 x 100: the badge at x LOBBY_X0 (the row's start inset), the
    name follows at LOBBY_X0 + reserved(40, 100). Hidden until t2r-ext (friends13.c fr_lobby_ranks) has a rank for the row."""
    n = 0
    for page in ("lobby-row", "find-match-row"):
        path, w = _load(wdir, page)
        items = w["items"]
        items[:] = [i for i in items if i["name"] not in ("rkIcon", "rkNum", "rkNumBox", "rkNumB", "rkNumCap")]
        root = next(i for i in items if i["name"] == "root")
        bg = next(i for i in items if i["name"] == "cardBg")
        nm = next(i for i in items if i["name"] == "nameLabel")
        pt = int(nm.get("pointSize", 40))
        x0 = LOBBY_X0
        add_to_items(items, root["guid"], bg, nm, "rkIcon", "rkNum", x0, 50.0, pt, 100.0, depth=2.0)
        nm["pivotAs.x"] = x0 + reserved(pt, 100.0)
        _save(path, w)
        n += 1
    return n


def add_scoreboards(wdir):
    """Every page with the fixed scoreboard rows team<t>_row<r>_name (pause-menu: the Esc scoreboard; scoreboard: the end-of-match page): a badge before each name
    (placed from the name's own, possibly restyled, box), the name moves right by reserved(pt, h). Call AFTER the pause-menu restyle."""
    import re
    n = 0
    for page in ("pause-menu", "scoreboard"):
        path, w = _load(wdir, page)
        items = w["items"]
        items[:] = [i for i in items if not re.match(r"team\d_row\d_rk(Icon|Num|NumBox|NumB|NumCap)$", i["name"])]
        names = [i for i in items if re.match(r"team\d_row\d_name$", i["name"])]
        if not names:
            continue
        img = next((i for i in items if i["name"].endswith("_row0_input") and i.get("class") == "Image"), None) or next(i for i in items if i.get("class") == "Image")
        for nm in names:
            if nm.get("anchorMinN.x", 0.0) != 0.0 or nm.get("anchorMinN.y", 0.0) != 0.0 or nm.get("anchorMaxN.x", 0.0) != 0.0 or nm.get("anchorMaxN.y", 0.0) != 0.0:
                raise SystemExit("rank badge: %s.%s is not top-left anchored" % (page, nm["name"]))
            pt = int(nm.get("pointSize", 38))
            h = float(nm.get("rectSizeDelta.height", 66.0))
            x = float(nm.get("pivotAs.x", 0.0)) - float(nm.get("pivotN.x", 0.0)) * float(nm.get("rectSizeDelta.width", 0.0))
            cy = float(nm.get("pivotAs.y", 0.0)) + (0.5 - float(nm.get("pivotN.y", 0.5))) * h
            pre = nm["name"][:-len("name")]
            ic, tx = add_to_items(items, nm["parent"], img, nm, pre + "rkIcon", pre + "rkNum", x, cy, pt, h, depth=float(nm.get("depth", 3.0)))
            nm["pivotAs.x"] = float(nm.get("pivotAs.x", 0.0)) + reserved(pt, h)
            if float(nm.get("rectSizeDelta.width", 0.0)) > reserved(pt, h) + 200:
                nm["rectSizeDelta.width"] = float(nm["rectSizeDelta.width"]) - reserved(pt, h)
            n += 1
        _save(path, w)
    return n


# The lead dev's name outline on the menu names (the name keeps its colour, only a hot pink #ff3d8b outline; ranks changed colour but the menu names did not - the first rim was only
# on the in-match overhead tags). gui2 text has no outline colour, so each name gets RIM_N hidden clones in pink at
# the 8 compass offsets * RIM_R, the same parent, depth just below the name. t2r-ext namerim.c fills / shows them for a role 4 row and keeps them on the name's
# place and scale (sbteam.c moves and shrinks scoreboard names).
RIM_ARGB = 0xFFFF537D                 # Discord Creator pink #ff537d (was #ff3d8b)
RIM_R = 1.0                  # = namerim.c NRIM_R (2.0 was too thick; the ext re-places the rims with its own value)
RIM_N = 8
RIM_DX = (1, 0.7071, 0, -0.7071, -1, -0.7071, 0, 0.7071)
RIM_DY = (0, 0.7071, 1, 0.7071, 0, -0.7071, -1, -0.7071)


def _rims(items, nm, pre):
    """RIM_N pink clones of the text item `nm`, named pre + "nmRim<i>", offset by (RIM_DX, RIM_DY) * RIM_R, depth just under it."""
    out = []
    for i in range(RIM_N):
        r = copy.deepcopy(nm)
        for k in ("cipherId", "animation", "effect"):
            r.pop(k, None)
        r["animation"] = {"keyframes": {}}
        r.update({"guid": t2guid.guid(), "name": "%snmRim%d" % (pre, i), "text": "", "visible": 0, "focusable": 0,
                  "depth": float(nm.get("depth", 2.0)) - 0.05, "color.a": 255, "tint.start": RIM_ARGB, "tint.end": RIM_ARGB, "tint.blend": 0.0, "inheritTint": 0,
                  "pivotAs.x": float(nm.get("pivotAs.x", 0.0)) + RIM_DX[i] * RIM_R, "pivotAs.y": float(nm.get("pivotAs.y", 0.0)) + RIM_DY[i] * RIM_R})
        out.append(r)
    items.extend(out)
    return out


def add_name_rims(wdir):
    """The lead dev's pink rim widgets under the names of the lobby rows (lobby-row / find-match-row nameLabel -> nmRim0..7) and both scoreboards
    (team<t>_row<r>_name -> team<t>_row<r>_nmRim0..7). Call AFTER add_lobby_rows / add_scoreboards (the names' final places). Returns the widget count."""
    import re
    n = 0
    for page in ("lobby-row", "find-match-row"):
        path, w = _load(wdir, page)
        items = w["items"]
        items[:] = [i for i in items if not re.match(r"nmRim\d$", i["name"])]
        nm = next(i for i in items if i["name"] == "nameLabel")
        n += len(_rims(items, nm, ""))
        _save(path, w)
    for page in ("pause-menu", "scoreboard"):
        path, w = _load(wdir, page)
        items = w["items"]
        items[:] = [i for i in items if not re.match(r"team\d_row\d_nmRim\d$", i["name"])]
        for nm in [i for i in items if re.match(r"team\d_row\d_name$", i["name"])]:
            n += len(_rims(items, nm, nm["name"][:-len("name")]))
        _save(path, w)
    return n
