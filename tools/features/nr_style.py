"""Shared No Return page frame for the UI redesign (re/ui-redesign-1117/PROPOSAL.md, re/ui-refs-1117/STYLE.md).

Every restyled page (GEAR UP, CLAN, PLAYER STATS, map vote, and the ACHIEVEMENTS / CUSTOMIZE pages of other branches) builds its frame
here, so the C side (tools/ext/nrui.c) finds the same widget names under anim-group on any page:

  backdrop   bgBlur (the engine's live menu blur), bgDim (black), bgVig (a little extra black at the left, one smooth image). No scratch texture.
  header     hdPar (grey caps) | hdDiv (hairline) | hdPage (white caps) and hdRule (hairline to R1). nrui.c nr_header re-places
             hdDiv / hdPage / hdRule from the measured (translated) text widths.
  tabs       Q glyph, tb0..tb7 labels, tbU underline, E glyph, tbHit0..7 (nrui.c nr_clan_tabs).
  week       wkDay "WEEK n: DAY n", wkSv "DAYS SURVIVED n | 84", wkBarT / wkBarF thin bar + wkTk1..11 ticks (nrui.c nr_week).
  prompts    pb0..pbN-1 boxed key prompts, bottom right (nrui.c nr_prompts lays them out right to left).

Widget space 3840 x 2160 = the 1920 x 1080 mockups (render.py) x 2. Point sizes: a mockup font of n px = pointSize 1.5 n
(widget_preview draws pointSize x 0.5 x 1.33 px at 1920).

Colours: two text colours (WHITE, GREY) + one accent (ACC, the NEW dot / EQUIPPED tag only)."""
import t2guid  # noqa: F401  (the Builder needs it; imported here so a page that only imports nr_style still has it on the path)

WHITE = 0xFFF0F0F0
GREY = 0xFF8C8C8C
DIMG = 0xFF585858          # a disabled prompt's label / key (about 40 % of white over the dim)
ACC = 0xFFD9D86A           # yellow-green: NEW, EQUIPPED
BLACK = 0xFF000000
INK = 0xFF0A0A0A           # dark text on a white chip / key box
TILE = 0xFF080808          # tile / row fill (alpha TILE_A unfocused, TILE_FA focused)
TILE_A, TILE_FA = 175, 215
HAIR_A = 45                # header / row rules
TRACK_A = 45               # scroll track: NR's track reads ~17 % white over the dim (nr-challenges-tracks), under a ~85 % thumb
HAIR = 4                   # hairline thickness: gui2 drops a rect under 1 screen px (2 units = 0.6 px in a 1216 window); 4 = NR's 2 px at 1080p
L0, R1 = 148, 3700         # page margins (mockup 74 / 1850)
HEAD_Y = 168               # breadcrumb centre line (mockup 84)
HEAD_PT = 54
TAB_Y, TAB_PT = 270, 36    # tab strip centre (mockup 134 / 24 px)
PROMPT_Y, PROMPT_H = 2000, 88
PROMPT_PT, PROMPT_KEY_PT = 36, 24
DIM_A = 165                # bgDim alpha: the scene at roughly a third of its brightness with the engine blur
VIG_IMG = "streaming/t2r/clan/nr-vig.png"   # bgVig texture (ship.py NR_VIG)
VIG_X0, VIG_W = -3840, 5640                # bgVig spans page x -3840 .. 1800 (= ship.make_nr_vig x0 / span)
NPROMPT = 6
MAX_TABS = 8
HIT_A = 1
RULE_W0 = 1000.0           # template width of every scaled rule / bar fill (nrui.c scales from it; previews set the real width)


def pt(px):
    """mockup font size (px at 1920) -> widget pointSize"""
    return round(px * 1.5)


def est(t, pts):
    """= nrui.c nr_est / gearup13.c gu_est: capitals / digits 0.80 pt, lower case 0.62 pt, space 0.40 pt (the game's face)."""
    import re
    t = re.sub("<[^>]*>", "", t)              # (colour tags are not drawn)
    return pts * sum(0.40 if c == " " else 0.62 if c.islower() or ord(c) > 127 else 0.80 for c in t)


def lm(x, y, w, h, right=0):
    """box (x, y, w, h) with its pivot at the left (right=1: right) middle: nrui.c / gu_fit scale a too-long text about it."""
    return {"anchorMinN.x": 0.0, "anchorMinN.y": 0.0, "anchorMaxN.x": 0.0, "anchorMaxN.y": 0.0,
            "pivotN.x": float(right), "pivotN.y": 0.5, "pivotAs.x": float(x + (w if right else 0)), "pivotAs.y": float(y + h / 2.0),
            "rectSizeDelta.width": float(w), "rectSizeDelta.height": float(h)}


def lpiv(x, y, w, h):
    """box with its pivot at the left middle: a bar fill scaled in x (w_scale) keeps its left end."""
    return {"anchorMinN.x": 0.0, "anchorMinN.y": 0.0, "anchorMaxN.x": 0.0, "anchorMaxN.y": 0.0,
            "pivotN.x": 0.0, "pivotN.y": 0.5, "pivotAs.x": float(x), "pivotAs.y": float(y + h / 2.0),
            "rectSizeDelta.width": float(w), "rectSizeDelta.height": float(h)}


# ------------------------------------------------------------------ frame parts
def backdrop(b, a):
    b.image("bgBlur", a, b.bleed(), "general-textures/blur-background.png", 0xFFFFFFFF, 255)
    b.rect("bgDim", a, b.bleed(), BLACK, DIM_A)
    # extra black at the left where the text sits: ONE image (tools/models/figures/ship.py make_nr_vig), the old six alpha-12 rects 250 apart
    # made smooth (the stacked rects showed as steps)
    b.image("bgVig", a, b.tl(VIG_X0, -2160, VIG_W, 2160 * 3), VIG_IMG, 0xFFFFFFFF, 255)


def header(b, a, parent, page):
    b.text("hdPar", a, lm(L0, HEAD_Y - 45, 1400, 90), parent, HEAD_PT, GREY, tracking=3)
    x = L0 + est(parent, HEAD_PT) * 1.06 + 52
    b.rect("hdDiv", a, b.tl(x, HEAD_Y - 34, HAIR, 68), WHITE, 70)
    b.text("hdPage", a, lm(x + 52, HEAD_Y - 45, 1800, 90), page, HEAD_PT, WHITE, tracking=3)
    x2 = x + 52 + est(page, HEAD_PT) * 1.06 + 52
    b.rect("hdRule", a, lpiv(x2, HEAD_Y - HAIR / 2, RULE_W0, HAIR), WHITE, HAIR_A)      # template width RULE_W0: nrui.c moves + scales it


def tab_layout(labels, x0=L0):
    """x / width of each tab label (= nrui.c nr_clan_tabs): the Q glyph, then the labels 80 apart, then the E glyph."""
    xs, x = [], x0 + 108
    for t in labels:
        w = est(t, TAB_PT) * 1.06 + 8
        xs.append((x, w))
        x += w + 80
    return xs, x - 80 + 28


def tabs(b, a, labels, active=0):
    g = b.group("tabs", a, b.tl(0, 0, 3840, 2160))["guid"]
    b.image("tbQ", g, b.ctr(L0 + 30, TAB_Y, 60, 56), "menu:prev", 0xFFFFFFFF)
    b.rect("tbQHit", g, b.ctr(L0 + 30, TAB_Y, 100, 110), WHITE, HIT_A)
    xs, ex = tab_layout(labels)
    for k in range(MAX_TABS):
        x, w = xs[k] if k < len(labels) else (xs[-1][0], 100)
        b.text(f"tb{k}", g, lm(x, TAB_Y - 36, 700, 72), labels[k] if k < len(labels) else "", TAB_PT, WHITE if k == active else GREY,
               tracking=3, visible=int(k < len(labels)))
        b.rect(f"tbHit{k}", g, b.tl(x - 30, TAB_Y - 60, w + 60, 120), WHITE, HIT_A, visible=int(k < len(labels)))
    x, w = xs[active]
    b.rect("tbU", g, lpiv(x, TAB_Y + 36, 100, 4), WHITE, 255)      # template width 100: nrui.c scales it to the label width
    b.image("tbE", g, b.ctr(ex + 30, TAB_Y, 60, 56), "menu:next", 0xFFFFFFFF)
    b.rect("tbEHit", g, b.ctr(ex + 30, TAB_Y, 100, 110), WHITE, HIT_A)
    return g


WK_X0, WK_W, WK_Y = R1 - 760, 760, 352


def week(b, a, week_n=1, day=1, survived=0):
    b.text("wkDay", a, lm(R1 - 1400, 256 - 36, 1400, 72, right=1), f"WEEK {week_n}: DAY {day}", pt(28), WHITE, align=2, tracking=3)
    b.text("wkSv", a, lm(R1 - 1400, 320 - 26, 1400, 52, right=1), f"DAYS SURVIVED {survived} | 84", pt(20), GREY, align=2, tracking=3)
    b.rect("wkBarT", a, b.tl(WK_X0, WK_Y, WK_W, 6), WHITE, 40)
    b.rect("wkBarF", a, lpiv(WK_X0, WK_Y, RULE_W0, 6), WHITE, 255)
    for k in range(1, 12):
        b.rect(f"wkTk{k}", a, b.tl(WK_X0 + WK_W * k / 12.0, WK_Y - 4, HAIR, 16), BLACK, 200)


# prompts: the key is an engine glyph (menu:select ... = the device's own key / button picture, the keyboard key in a white box like
# No Return's) or, for a key with no menu command (W / S, A / D), white key text in a white box. nrui.c nr_prompts writes all of it.
PB_W0 = 400.0                  # template width of the scaled outline pieces


def prompt_slots(b, a, n=NPROMPT):
    g0 = b.group("pbar", a, b.tl(0, 0, 3840, 2160))["guid"]
    for k in range(n):
        g = b.group(f"pb{k}", g0, b.tl(R1 - 420 * (k + 1), PROMPT_Y - PROMPT_H / 2, PB_W0, PROMPT_H), visible=0)["guid"]
        b.rect(f"pb{k}T", g, lpiv(0, 0, PB_W0, HAIR), WHITE, 70)
        b.rect(f"pb{k}B", g, lpiv(0, PROMPT_H - HAIR, PB_W0, HAIR), WHITE, 70)
        b.rect(f"pb{k}L", g, b.tl(0, 0, HAIR, PROMPT_H), WHITE, 70)
        b.rect(f"pb{k}R", g, b.tl(PB_W0 - HAIR, 0, HAIR, PROMPT_H), WHITE, 70)
        b.image(f"pb{k}Kg", g, b.tl(18, 16, 64, 56), "menu:select", 0xFFFFFFFF, scaleMode="StretchAspect")
        b.image(f"pb{k}Kg2", g, b.tl(90, 16, 64, 56), "menu:next", 0xFFFFFFFF, scaleMode="StretchAspect", visible=0)
        b.rect(f"pb{k}Kb", g, lpiv(18, 16, 100, 56), 0xFFE6E6E6, 255, visible=0)
        b.text(f"pb{k}Kt", g, b.tl(18, 16, 100, 56), "", PROMPT_KEY_PT, INK, align=1, tracking=1, visible=0)
        b.text(f"pb{k}Tx", g, lm(100, 0, 900, PROMPT_H), "", PROMPT_PT, WHITE, tracking=2)
        b.rect(f"pb{k}Hit", g, b.fill(), WHITE, HIT_A)
    return g0


def frame(b, a, parent, page, tabs_labels=None, active_tab=0, wk=None, prompts=NPROMPT):
    """The whole frame. tabs_labels: the Q / E strip (None = none). wk: (week, day, survived) for the top right block (None = none)."""
    backdrop(b, a)
    header(b, a, parent, page)
    if tabs_labels:
        tabs(b, a, tabs_labels, active_tab)
    if wk:
        week(b, a, *wk)
    if prompts:
        prompt_slots(b, a, prompts)


# ------------------------------------------------------------------ content parts
def section(b, g, name, x0, x1, y, title, right="", pts=None, col=None):
    """Row header (= the page header's grammar): grey caps title, a hairline, an optional grey count / note right aligned.
    <name>Tx, <name>Rt, <name>Rl. nrui.c nr_section re-places the rule from the measured widths."""
    pts = pts or pt(24)
    b.text(f"{name}Tx", g, lm(x0, y - 30, 1600, 60), title, pts, col or GREY, tracking=3)
    b.text(f"{name}Rt", g, lm(x1 - 1600, y - 28, 1600, 56, right=1), right, pt(22), GREY, align=2, tracking=2)
    xa = x0 + est(title, pts) * 1.06 + 40
    xb = x1 - (est(right, pt(22)) * 1.06 + 40 if right else 0)
    b.rect(f"{name}Rl", g, lpiv(xa, y - HAIR / 2, RULE_W0, HAIR), WHITE, 40)
    return xa, xb


def bar(b, g, name, x0, x1, y, h=6, ticks=0, tick_col=BLACK):
    """Thin bar: <name>T track, <name>F fill (left pivot, template width RULE_W0: nrui.c nr_bar scales it to the fraction), ticks
    <name>K1.. (dark gaps, like the LP / stat bars)."""
    b.rect(f"{name}T", g, b.tl(x0, y, x1 - x0, h), WHITE, 40)
    b.rect(f"{name}F", g, lpiv(x0, y, RULE_W0, h), WHITE, 255)
    for t in range(1, ticks):
        b.rect(f"{name}K{t}", g, b.tl(x0 + (x1 - x0) * t / ticks, y - 4, HAIR, h + 8), tick_col, 210)


def tile(b, g, name, w, h, focus=False):
    """A tile / row card: <name>Bg dim fill, <name>Fo focus outline (2 px white, 4 rects so nothing scales). Children go in g."""
    b.rect(f"{name}Bg", g, b.fill(), TILE, TILE_FA if focus else TILE_A)
    fo = b.group(f"{name}Fo", g, b.fill(), visible=int(focus))["guid"]
    b.rect(f"{name}FoT", fo, b.tl(0, 0, w, 4), WHITE, 255)
    b.rect(f"{name}FoB", fo, b.tl(0, h - 4, w, 4), WHITE, 255)
    b.rect(f"{name}FoL", fo, b.tl(0, 0, 4, h), WHITE, 255)
    b.rect(f"{name}FoR", fo, b.tl(w - 4, 0, 4, h), WHITE, 255)


def focus_frame(b, g, name, th=4, alpha=255, visible=0):
    """ONE movable focus outline for a whole tile set (widget budget: no outline per tile): <name>T / B (left pivot, template width 100),
    <name>L / R (top-left pivot, template height 100). nrui.c nr_frame_at(name, x, y, w, h) places + scales the four sides."""
    b.rect(f"{name}T", g, lpiv(0, 0, 100, th), WHITE, alpha, visible=visible)
    b.rect(f"{name}B", g, lpiv(0, 100 - th, 100, th), WHITE, alpha, visible=visible)
    b.rect(f"{name}L", g, b.tl(0, 0, th, 100), WHITE, alpha, visible=visible)
    b.rect(f"{name}R", g, b.tl(100 - th, 0, th, 100), WHITE, alpha, visible=visible)


def demo_frame(by, name, x, y, w, h, th=4, on=True):
    for s in "TBLR":
        by[f"{name}{s}"]["visible"] = int(on)
    by[f"{name}T"].update({"pivotAs.x": float(x), "pivotAs.y": float(y + th / 2.0), "rectSizeDelta.width": float(w)})
    by[f"{name}B"].update({"pivotAs.x": float(x), "pivotAs.y": float(y + h - th / 2.0), "rectSizeDelta.width": float(w)})
    by[f"{name}L"].update({"pivotAs.x": float(x), "pivotAs.y": float(y), "rectSizeDelta.height": float(h)})
    by[f"{name}R"].update({"pivotAs.x": float(x + w - th), "pivotAs.y": float(y), "rectSizeDelta.height": float(h)})


def plain_tile(b, g, name, x, y, w, h, alpha=TILE_A, visible=1):
    """A tile without its own outline (the tab's focus_frame marks the focused one): group <name> at (x, y) + <name>Bg. -> the group's guid."""
    t = b.group(name, g, b.tl(x, y, w, h), visible=visible)["guid"]
    b.rect(f"{name}Bg", t, b.fill(), TILE, alpha)
    return t


CHIP_W0 = 100
CHIPS = {}                                      # chip name -> its built text width (demo_chips puts it into a preview)


def demo_chips(by):
    for n, w in CHIPS.items():
        if n + "Bg" in by:
            by[n + "Bg"]["rectSizeDelta.width"] = w


def chip(b, g, name, x, y, text, pts=None, style="white", w=None):
    """Tag chip: <name>Bg (white / grey / accent box), <name>Tx dark text. w = box width (default from the text)."""
    pts = pts or pt(20)
    w = w or est(text, pts) * 1.06 + 32
    h = pts * 1.05 + 14
    fill = {"white": 0xFFEBEBEB, "grey": 0xFF8C8C8C, "acc": ACC, "dark": 0xFF282828}[style]
    bg = b.rect(f"{name}Bg", g, lpiv(x, y, CHIP_W0, h), fill, 255)          # (template width CHIP_W0: nrui.c / clan13.c scale it to the text; the preview gets the real width here)
    CHIPS[name] = float(w)
    b.text(f"{name}Tx", g, lm(x + 16, y, 1100, h), text, pts, INK if style != "dark" else WHITE, tracking=1)
    return w


def dot(b, g, name, cx, cy, r=12, col=ACC, visible=1):
    e = b.ellipse(name, g, b.ctr(cx, cy, 2 * r, 2 * r), col, 255)
    e["visible"] = int(bool(visible))
    return e


def scroll_v(b, g, name, x, y0, y1, frac0=0.0, frac1=1.0, visible=1):
    """Vertical hairline track + white thumb (left pivot is not needed: nrui.c moves / scales the thumb)."""
    b.rect(f"{name}T", g, b.tl(x + 1 - HAIR / 2, y0, HAIR, y1 - y0), WHITE, TRACK_A, visible=visible)      # centred on the thumb (x - 2 .. x + 4)
    b.rect(f"{name}H", g, b.tl(x - 2, y0 + (y1 - y0) * frac0, 6, max(8.0, (y1 - y0) * (frac1 - frac0))), WHITE, 230, visible=visible)


def scroll_h(b, g, name, x0, x1, y, frac0=0.0, frac1=1.0, visible=1):
    b.rect(f"{name}T", g, b.tl(x0, y + 1 - HAIR / 2, x1 - x0, HAIR), WHITE, TRACK_A, visible=visible)      # centred on the thumb (y - 2 .. y + 4)
    b.rect(f"{name}H", g, lpiv(x0, y - 2, RULE_W0, 6), WHITE, 230, visible=visible)      # nrui.c nr_scroll_h moves + scales it


# ------------------------------------------------------------------ previews
def demo_prompts(by, items):
    """Offline stand-in for nrui.c nr_prompts: items = [(key, label, on, glyph)], right to left from R1. glyph = a menu:* id (drawn
    as the engine glyph) or None (the key text in a white box)."""
    x = R1
    for k in range(NPROMPT):
        if f"pb{k}" in by:
            by[f"pb{k}"]["visible"] = 0
    for k, (key, label, on, glyph) in enumerate(reversed(items)):
        lw = est(label, PROMPT_PT) * 1.06
        kw = 64 if glyph else max(60.0, est(key, PROMPT_KEY_PT) * 1.06 + 28)
        tot = 18 + kw + 22 + lw + 34
        g = by[f"pb{k}"]
        g["visible"] = 1
        g["pivotAs.x"] = float(x - tot)
        g["pivotAs.y"] = float(PROMPT_Y - PROMPT_H / 2)
        for s in ("T", "B"):
            by[f"pb{k}{s}"]["rectSizeDelta.width"] = float(tot)
        by[f"pb{k}Hit"]["visible"] = 1
        by[f"pb{k}R"]["pivotAs.x"] = float(tot - 2)
        a = 70 if on else 30
        for s in "TBLR":
            by[f"pb{k}{s}"]["color.a"] = a
        by[f"pb{k}Kg"]["visible"] = int(bool(glyph))
        by[f"pb{k}Kg2"]["visible"] = 0
        by[f"pb{k}Kb"]["visible"] = int(not glyph)
        by[f"pb{k}Kt"]["visible"] = int(not glyph)
        if glyph:
            by[f"pb{k}Kg"]["path"] = glyph
            by[f"pb{k}Kg"]["color.a"] = 255 if on else 100
        else:
            by[f"pb{k}Kb"]["rectSizeDelta.width"] = float(kw)
            by[f"pb{k}Kb"]["color.a"] = 255 if on else 100
            by[f"pb{k}Kt"]["rectSizeDelta.width"] = float(kw)
            by[f"pb{k}Kt"]["text"] = key
        tx = by[f"pb{k}Tx"]
        tx["text"] = label
        tx["pivotAs.x"] = float(18 + kw + 22)
        tx["tint.start"] = tx["tint.end"] = WHITE if on else DIMG
        x -= tot + 28


def demo_header(by, parent, page):
    by["hdPar"]["text"] = parent
    x = L0 + est(parent, HEAD_PT) * 1.06 + 52
    by["hdDiv"]["pivotAs.x"] = float(x)
    by["hdPage"]["text"] = page
    by["hdPage"]["pivotAs.x"] = float(x + 52)
    x2 = x + 52 + est(page, HEAD_PT) * 1.06 + 52
    by["hdRule"]["pivotAs.x"] = float(x2)
    by["hdRule"]["rectSizeDelta.width"] = float(R1 - x2)


def demo_bar(by, name, x0, x1, frac):
    by[f"{name}F"]["rectSizeDelta.width"] = float(max(1.0, (x1 - x0) * max(0.0, min(1.0, frac))))
    by[f"{name}F"]["visible"] = int(frac > 0)


def demo_rule(by, name, xa, xb):
    by[name]["pivotAs.x"] = float(xa)
    by[name]["rectSizeDelta.width"] = float(max(2.0, xb - xa))


def demo_section(by, name, x0, x1, title, right=""):
    by[f"{name}Tx"]["text"] = title
    by[f"{name}Rt"]["text"] = right
    xa = x0 + est(title, by[f"{name}Tx"]["pointSize"]) * 1.06 + 40
    xb = x1 - (est(right, by[f"{name}Rt"]["pointSize"]) * 1.06 + 40 if right else 0)
    demo_rule(by, f"{name}Rl", xa, xb)


def demo_scroll_h(by, name, x0, x1, frac0, frac1):
    by[f"{name}H"]["pivotAs.x"] = float(x0 + (x1 - x0) * frac0)
    by[f"{name}H"]["rectSizeDelta.width"] = float(max(8.0, (x1 - x0) * (frac1 - frac0)))


def demo_tabs(by, labels, active):
    xs, ex = tab_layout(labels)
    for k in range(MAX_TABS):
        on = k < len(labels)
        by[f"tb{k}"]["visible"] = int(on)
        by[f"tbHit{k}"]["visible"] = int(on)
        if on:
            by[f"tb{k}"]["text"] = labels[k]
            by[f"tb{k}"]["pivotAs.x"] = float(xs[k][0])
            c = WHITE if k == active else GREY
            by[f"tb{k}"]["tint.start"] = by[f"tb{k}"]["tint.end"] = c
    by["tbU"]["pivotAs.x"] = float(xs[active][0])
    by["tbU"]["rectSizeDelta.width"] = float(xs[active][1])
    by["tbE"]["pivotAs.x"] = float(ex + 30)
    by["tbEHit"]["pivotAs.x"] = float(ex + 30)


def demo_week(by, week_n, day, survived):
    by["wkDay"]["text"] = f"WEEK {week_n}: DAY {day}"
    by["wkSv"]["text"] = f"DAYS SURVIVED {survived} | 84"
    by["wkBarF"]["rectSizeDelta.width"] = float(max(1.0, WK_W * survived / 84.0))


def preview_bg(out_png, name="haven-boardwalk.jpg"):
    """The mockups' backdrop for offline previews (the in-game one is the engine's live blur of the menu scene): blurred launcher art at
    full brightness - the page's own bgDim / bgVig are drawn over it by widget_preview."""
    import os
    from PIL import Image, ImageFilter
    root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    src = os.path.join(root, "release", "launcher", "assets", "bg", name)
    if not os.path.exists(src):
        src = os.path.join(".", "release", "launcher", "assets", "bg", name)
    im = Image.open(src).convert("RGB")
    g = im.convert("L").point(lambda v: 255 if v > 12 else 0)
    im = im.crop(g.getbbox() or (0, 0, im.width, im.height))
    W, H = 1920, 1080
    s = max(W / im.width, H / im.height)
    im = im.resize((int(im.width * s) + 1, int(im.height * s) + 1), Image.LANCZOS)
    x, y = (im.width - W) // 2, (im.height - H) // 2
    im = im.crop((x, y, x + W, y + H)).filter(ImageFilter.GaussianBlur(11))
    im.save(out_png)
    return out_png
