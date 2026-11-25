"""CUSTOMIZE page (mockup re/customize/mockup-user-1117.png; doc re/customize-1117.md): to1/customize, opened from the
CUSTOMIZE row under CLAN on the MP main menu (t2r-ext customize.c).

Left side = the No Return CHARACTER SELECT | SKINS page, element for element (this type of ui on the left should be
exactly what we need"; stock widgets t2r-menu-character-select/tabs-skin-select-rogue-mode + t2r-character-select-skin-slot, screenshot
re/customize-1117/nr-skins-tab.png):
  header      grey "CUSTOMIZE" | white category name, a thin rule to the right edge (the stock title-group)
  tab bar     Q glyph - line - one icon per category (the open one white, the others faded to the stock fader alpha 84) - line - E glyph
  item row    the focused item's name + a thin rule (the stock skinDetails: itemName + lineTop)
  grid        3 columns of 300 x 275 slots, 32 apart (the stock skin slot: blurred backdrop, near-black fill, the lit fill + white lines
              top and bottom when focused, the white disc + black check bottom right = equipped, a padlock top left = not in MP yet)
  prompts     bottom right: SWITCH FACTION / EQUIP / BACK (the stock RANDOM / SELECT / BACK row)
Right: the two live player models stand in the menu world (3D, t2r-ext); this page only carries what sits under them: the faction
logos (the game's own photo-mode logos, as the faction pick) - the front one under the spotlit model, the back one dimmed - and the
white disc with the black check at the bottom right of the logo of the faction the player plays as. czHitF / czHitB = click a model.
Widget space 3840 x 2160. python customize_widgets.py <extracted widget dir> <out dir> renders the previews."""
import copy
import json
import os
import t2guid

from clan_widgets import Builder, _templates
from gearup13_widgets import HIT_A
import nr_style as nr


def tlm(x, y, w, h, right=0):
    """Box (x, y, w, h) with its pivot at the left (right=1: right) middle: a shrunk name keeps its left (right) edge and its
    vertical centre (was gearup13_widgets.tlm before the NR frame redesign)."""
    return {"anchorMinN.x": 0.0, "anchorMinN.y": 0.0, "anchorMaxN.x": 0.0, "anchorMaxN.y": 0.0,
            "pivotN.x": float(right), "pivotN.y": 0.5, "pivotAs.x": float(x + (w if right else 0)), "pivotAs.y": float(y + h / 2.0),
            "rectSizeDelta.width": float(w), "rectSizeDelta.height": float(h)}

PAGE = "customize"
WHITE = 0xFFFFFFFF
PREV = 0xFF505050                   # the stock t2-menu-header-previous grey (measured 80 / 255 on the SKINS page)
LINE_A = 46                         # t2-menu-line: the thin rules (measured ~19 / 255 over black at 1080p, 2 px at 4K)
PROMPT = 0xFFD0CFCE                 # the stock prompt text (208)
DIM = 0xFF8E8A84
ICONS = "streaming/t2r/icons/"
CATS = ("hat", "torso", "lower", "backpack", "face", "melee", "mask", "sex")      # = customize_items.CATS (the saved order; tabIc<i> = CATS[i])
TAB_ORDER = ("hat", "mask", "torso", "lower", "backpack", "face", "melee", "sex")   # = customize_items.TAB_ORDER: the places on the bar
LOGO = {"wlf": "streaming/photo-mode/logos/wlf-wolf-logo.png", "scar": "streaming/photo-mode/logos/scar-logo.png"}   # = choose_widgets.LOGO

# geometry (canvas 3840 x 2160) = the stock page: tabs-skin-select-rogue-mode is centred, its groups sit at (-1729, -608) / (-1715, -558)
# / tab bar centre (-1187, -719) from the screen centre. customize.c CZ_* repeat these numbers.
HEAD_X, HEAD_CY, HEAD_PT = 146, 166, 66
HEAD_DIV_X, HEAD_SUB_X, HEAD_RULE_X0, RIGHT_X = 678, 744, 1240, 3700     # (t2r-ext re-places the divider / subtitle / rule from
                                                                         # the drawn text widths: the stock flow group, 65 apart)
BAR_CY = 361                        # tab bar
BAR_Q_X, BAR_LL, BAR_IC_X0, BAR_CELL, BAR_GAP = 174, (226, 340), 372, 88, 34
BAR_IC = 76                         # icon box
BAR_E_X = 1336 + 122                # (+ one cell + gap: the eighth tab, MASK)
BAR_LR = (1224 + 122, 1290 + 122)
TAB_A_OFF = 84                      # the stock fader alpha of a closed tab's icon
FAC_Y, FAC_H, FAC_PAD = HEAD_CY + 20, 72, 32       # the other-faction note's box (top right, under the header rule) = customize.c CZ_FAC_*
FAC_W = 1240                        # its laid-out width (the English note at 30 pt + the padding); customize.c fits it to the text
FAC_LINE_A = 110
ITEM_X, ITEM_CY, ITEM_PT = 191, 497, 56
ITEM_RULE_X1 = 1310
GRID_X, GRID_Y, CARD_W, CARD_H, GAP = 205, 615, 300, 275, 32
COLS, ROWS = 3, 4
NCARD = COLS * ROWS
SB_X = GRID_X + COLS * CARD_W + (COLS - 1) * GAP + 30     # scroll bar
PROMPT_CY, PROMPT_PT, GLYPH = 2002, 46, 56
CH_KEY, CH_W = 52, 420             # the CHALLENGES prompt: key box, label width
ZM_KEY, ZM_W = 84, 300             # the ZOOM prompt: key box ("R3" on a pad), label width ("ZOOM OUT")
UO_X0, UO_KEY, UO_W, UO_PT = BAR_E_X + GLYPH // 2 + 70, 84, 400, 32   # the UNLOCKED ONLY toggle on the tab bar (ends ~2056, left of the front model)
# the prompts, right to left from RIGHT_X (the stock row: glyph, 16 gap, label; 80 between prompts)
PROMPTS = (("pBack", "menu:back", "BACK", 180), ("pSel", "menu:select", "EQUIP", 200), ("pSw", "menu:action", "SWITCH FACTION", 480))
# the two models (3D, in the menu world): front = the selected one in the spotlight, back = further back, in shadow (the mockup)
FRONT = dict(cx=2330, logo_cy=1640, logo_h=230, hit=(1880, 300, 900, 1250))
BACK = dict(cx=3380, logo_cy=1420, logo_h=150, hit=(3020, 470, 720, 860))
BACK_A = 110                        # the back logo's alpha (shadow)


# the unlock requirement card (the unlock requirement text cut into other elements; the approved
# card): under the grid at the grid's width, so it never reaches the prompt row (prompts start right of x 1890). Lock + the challenge
# title in small caps (beige), the description in grey (multiline, as many lines as it needs), a thin bar with n / N on its right.
# An unlocked item: one line, check + UNLOCKED. customize.c cz_req_fill places the bar under the last description line (CZ_REQ_*).
REQ_X0, REQ_X1 = GRID_X, GRID_X + COLS * CARD_W + (COLS - 1) * GAP       # 205 .. 1169
REQ_Y = GRID_Y + ROWS * CARD_H + (ROWS - 1) * GAP + 36                 # 1847: the icon row's top, 36 under the grid
REQ_IC = 34                         # lock / check icon
REQ_TI_PT, REQ_DS_PT, REQ_N_PT = nr.pt(19), nr.pt(21), nr.pt(19)
REQ_DS_Y = REQ_Y + 56               # the description's first line top
REQ_LINE_H = 60                     # one description line (customize.c CZ_REQ_LINE_H): ~1.86 x pointSize, measured on GEAR UP's
                                    # two-line lsDesc in game (pt 36: 67 units)
REQ_MAX_LINES = 3                   # what fits above the screen edge (test_customize_req: every EN / PL line wraps to 3 at most)
REQ_BAR_H = 6                       # the thin bar (3 px at 1080p)
REQ_N_W = 260                       # n / N box, right aligned at REQ_X1 (the bar ends 36 before the drawn count)
REQ_BEIGE = 0xFFCDBEA0
REQ_GREY = 0xFFB0B0B0


def build_req(b, a):
    g = b.group("req", a, b.tl(0, 0, 3840, 2160), visible=0)["guid"]
    b.image("reqLk", g, b.tl(REQ_X0, REQ_Y, REQ_IC, REQ_IC), ICONS + "cz-lock.png", REQ_BEIGE, 255)
    b.image("reqOk", g, b.tl(REQ_X0, REQ_Y, REQ_IC, REQ_IC), ICONS + "cz-check.png", 0xFFE6E6E6, 255, visible=0)
    b.text("reqTi", g, tlm(REQ_X0 + REQ_IC + 18, REQ_Y + REQ_IC / 2 - 30, REQ_X1 - REQ_X0 - REQ_IC - 18, 60), "", REQ_TI_PT,
           REQ_BEIGE, tracking=5)
    b.text("reqDs", g, b.tl(REQ_X0, REQ_DS_Y, REQ_X1 - REQ_X0, 6 * REQ_LINE_H), "", REQ_DS_PT, REQ_GREY, tracking=1, multiline=1,
           valign=0)
    y = REQ_DS_Y + 2 * REQ_LINE_H + 22                                  # (template: two lines; customize.c moves it)
    b.rect("reqBT", g, nr.lpiv(REQ_X0, y, nr.RULE_W0, REQ_BAR_H), WHITE, 46)          # track (scaled to its length by the ext)
    b.rect("reqBF", g, nr.lpiv(REQ_X0, y, nr.RULE_W0, REQ_BAR_H), WHITE, 255)         # fill (nrui.c nr_bar)
    b.text("reqN", g, tlm(REQ_X1 - REQ_N_W, y + REQ_BAR_H / 2 - 30, REQ_N_W, 60, right=1), "", REQ_N_PT, 0xFFE6E6E6, align=2,
           tracking=2)


def _cw(ch):
    """= customize.c cz_req_cw: the estimated width of one character in pointSize units (nrui.c nr_cw; any non-ASCII letter counts as
    a capital - the descriptions are upper case: Ż, Ó, Ą ...)."""
    return 0.40 if ch == " " else 0.62 if "a" <= ch <= "z" else 0.80


def req_lines(t, pt=REQ_DS_PT, w=REQ_X1 - REQ_X0):
    """= customize.c cz_req_lines: greedy word wrap on the estimated widths (+6 % tracking, 4 % slack): how many lines t takes."""
    sp, lim, lines, x = _cw(" ") * pt * 1.06, w * 0.96, 1, 0.0
    for word in t.split():
        ww = sum(_cw(c) for c in word) * pt * 1.06
        if x > 0 and x + sp + ww > lim:
            lines, x = lines + 1, ww
        else:
            x += (sp if x > 0 else 0) + ww
    return lines


def req_bar_y(lines):
    """= customize.c: the bar's top, under the last description line."""
    return REQ_DS_Y + min(lines, REQ_MAX_LINES) * REQ_LINE_H + 22


def req_demo(by, title, desc, n, N, unlocked):
    """Preview: the card as customize.c cz_req_fill fills it."""
    by["req"]["visible"] = 1
    by["reqOk"]["visible"], by["reqLk"]["visible"] = int(unlocked), int(not unlocked)
    by["reqTi"]["text"] = "UNLOCKED" if unlocked else title
    for k in ("reqDs", "reqBT", "reqBF", "reqN"):
        by[k]["visible"] = int(not unlocked)
    if unlocked:
        return
    by["reqDs"]["text"] = desc
    y = req_bar_y(req_lines(desc)) + REQ_BAR_H / 2
    cnt = "%d / %d" % (min(n, N), N)
    by["reqN"]["text"] = cnt
    by["reqN"]["pivotAs.y"] = y
    x1 = REQ_X1 - len(cnt) * 0.8 * REQ_N_PT * 1.06 - 36
    for k, frac in (("reqBT", 1.0), ("reqBF", min(1.0, n / N if N else 0))):
        by[k]["pivotAs.y"] = y
        by[k]["rectSizeDelta.width"] = max(2.0, (x1 - REQ_X0) * frac)
    by["reqBF"]["visible"] = int(n > 0)


def tab_cx(i):
    return BAR_IC_X0 + BAR_CELL / 2 + i * (BAR_CELL + BAR_GAP)


def card_xy(i):
    return GRID_X + (i % COLS) * (CARD_W + GAP), GRID_Y + (i // COLS) * (CARD_H + GAP)


def prompt_boxes():
    """(name, glyph, label, glyph x, label x, label w), right to left."""
    out, x = [], RIGHT_X
    for nm, gl, tx, w in PROMPTS:
        lx = x - w
        gx = lx - 16 - GLYPH
        out.append((nm, gl, tx, gx, lx, w))
        x = gx - 80
    return out


def build_page(main):
    t, root0, anim0 = _templates(main)
    b = Builder(t)
    root = copy.deepcopy(root0); root.update(guid=t2guid.guid())
    anim = copy.deepcopy(anim0); anim.update(guid=t2guid.guid(), parent=root["guid"])
    b.items += [root, anim]
    a = anim["guid"]
    # the left shade: the stock page dims the whole scene; here the right half shows the live models, so the shade fades out
    # towards them in steps (no hard edge)
    b.rect("bgLeft", a, b.tl(-1920, -1080, 1920 + 1300, 2160 * 2), 0xFF000000, 150)
    for k in range(8):
        b.rect(f"bgFade{k}", a, b.tl(1300 + k * 60, -1080, 60, 2160 * 2), 0xFF000000, int(150 * (1 - (k + 1) / 9)))
    # ------------------------------------------------------------ header: CUSTOMIZE | <category> + rule
    b.text("title", a, tlm(HEAD_X, HEAD_CY - 45, 440, 90), "CUSTOMIZE", HEAD_PT, PREV, tracking=2)
    b.rect("titleDiv", a, b.tl(HEAD_DIV_X - 1, HEAD_CY - 25, 2, 50), WHITE, LINE_A * 2)
    b.text("titleSub", a, tlm(HEAD_SUB_X, HEAD_CY - 45, 900, 90), "TORSO", HEAD_PT, WHITE, tracking=2)
    b.rect("titleRule", a, b.tl(HEAD_RULE_X0, HEAD_CY - 1, RIGHT_X - HEAD_RULE_X0, 2), WHITE, LINE_A)
    # ------------------------------------------------------------ tab bar: Q - icons - E
    tabs = b.group("tabs", a, b.tl(0, 0, 3840, 2160))["guid"]
    b.image("tabQ", tabs, b.ctr(BAR_Q_X, BAR_CY, GLYPH, GLYPH), "menu:prev", WHITE)
    b.rect("tabQHit", tabs, b.ctr(BAR_Q_X, BAR_CY, 100, 100), WHITE, HIT_A)
    b.line("tabLineL", tabs, BAR_LL[0], BAR_CY, BAR_LL[1], BAR_CY, WHITE, LINE_A * 2, 2.0)
    for i, c in enumerate(CATS):
        x = tab_cx(TAB_ORDER.index(c))
        b.image(f"tabIc{i}", tabs, b.ctr(x, BAR_CY, BAR_IC, BAR_IC), ICONS + f"cz-{c}.png", WHITE, TAB_A_OFF, scaleMode="StretchAspect")
        b.rect(f"tabHit{i}", tabs, b.ctr(x, BAR_CY, BAR_CELL + BAR_GAP, 110), WHITE, HIT_A)
    b.line("tabLineR", tabs, BAR_LR[0], BAR_CY, BAR_LR[1], BAR_CY, WHITE, LINE_A * 2, 2.0)
    b.image("tabE", tabs, b.ctr(BAR_E_X, BAR_CY, GLYPH, GLYPH), "menu:next", WHITE)
    b.rect("tabEHit", tabs, b.ctr(BAR_E_X, BAR_CY, 100, 100), WHITE, HIT_A)
    # UNLOCKED ONLY (customize.c g_cz_uonly): U on the keyboard, L3 on the pad, a click. The grid of every category lists only the
    # unlocked items while it is on. Right of the E glyph on the tab bar (the prompt row's left part belongs to the requirement card);
    # the label names what a press does (as ZOOM IN / ZOOM OUT): UNLOCKED ONLY, then SHOW ALL
    b.rect("tabUoKb", tabs, b.tl(UO_X0, BAR_CY - CH_KEY / 2, UO_KEY, CH_KEY), 0xFFE6E6E6, 255)
    b.text("tabUoKt", tabs, b.tl(UO_X0, BAR_CY - CH_KEY / 2, UO_KEY, CH_KEY), "U", 34, 0xFF141414, align=1, tracking=1)
    b.text("tabUoTx", tabs, tlm(UO_X0 + UO_KEY + 16, BAR_CY - 35, UO_W, 70), "UNLOCKED ONLY", UO_PT, PROMPT, tracking=2)
    b.rect("tabUoHit", tabs, b.tl(UO_X0 - 16, BAR_CY - 55, UO_KEY + 16 + UO_W + 32, 110), WHITE, HIT_A)
    # ------------------------------------------------------------ item row: the focused item's name + rule (t2r-ext moves the rule's
    # left end to just past the name: secRule is a 2 px rect, pivot at its left end, scaled in x)
    b.text("secName", a, tlm(ITEM_X, ITEM_CY - 40, 1000, 80), "TORSO 10", ITEM_PT, WHITE, tracking=2)
    b.rect("secRule", a, b.tl(ITEM_X + 500, ITEM_CY - 1, ITEM_RULE_X1 - ITEM_X - 500, 2), WHITE, LINE_A)
    # ------------------------------------------------------------ grid of slots
    grid = b.group("grid", a, b.tl(0, 0, 3840, 2160))["guid"]
    for i in range(NCARD):
        x, y = card_xy(i)
        g = b.group(f"cd{i}", grid, b.tl(x, y, CARD_W, CARD_H))["guid"]
        b.image(f"cdBlur{i}", g, b.fill(), "general-textures/blur-background.png", WHITE, 255)
        b.rect(f"cdCard{i}", g, b.fill(), 0xFF000000, 200)                     # the stock slot-grey-normal: near black
        b.rect(f"cdFill{i}", g, b.fill(), 0xFF2A2826, 255, visible=0)          # slot-grey-focused (measured 25, 24, 23)
        b.image(f"cdIc{i}", g, b.ctr(CARD_W / 2, 122, 150, 150), ICONS + "cz-torso.png", WHITE, 150, scaleMode="StretchAspect")
        # the item's picture (tools/models/customize_cards.py: a textured render of its parts, the card's size, the render fitted
        # above the name); an item without one shows the category icon (cdIc) instead
        b.image(f"cdPic{i}", g, b.fill(), ICONS + "cz-torso.png", WHITE, 255, visible=0)
        # (the box: 226 wide. At 190 a single word over about 8 letters broke inside the word - e.g. "WEATHERE / D / RAINCOAT",
        # "ROLLED-SL / EEVE / SHIRT". The names still break by the 190 rule (cz_card_label), so only such a word reaches into the
        # 36 px next to the equipped check.)
        b.text(f"cdNm{i}", g, b.tl(22, 196, CARD_W - 74, 70), "", 24, DIM, tracking=1, multiline=1, valign=2)
        # (two lines at most, bottom aligned, clear of the check mark)
        b.rect(f"cdTop{i}", g, b.tl(0, -1, CARD_W, 2), 0xFFDADADA, 255, visible=0)       # focused: white lines top and bottom
        b.rect(f"cdBot{i}", g, b.tl(0, CARD_H - 1, CARD_W, 2), 0xFFDADADA, 255, visible=0)
        b.image(f"cdEq{i}", g, b.tl(CARD_W - 24 - 64, CARD_H - 10 - 64, 64, 64), ICONS + "cz-check.png", WHITE, 255, visible=0)
        # the equipped check of an OG card (its badge sits bottom right: customize_cards.OG_BOX): top right
        b.image(f"cdEqU{i}", g, b.tl(CARD_W - 24 - 64, 10, 64, 64), ICONS + "cz-check.png", WHITE, 255, visible=0)
        b.image(f"cdLock{i}", g, b.tl(24, 10, 64, 64), ICONS + "cz-lock.png", WHITE, 255, visible=0)
        b.rect(f"cdHit{i}", g, b.fill(), WHITE, HIT_A)
    gh = ROWS * CARD_H + (ROWS - 1) * GAP
    b.rect("sbTrack", grid, b.tl(SB_X, GRID_Y, 4, gh), WHITE, 30, visible=0)
    b.rect("sbThumb", grid, b.tl(SB_X, GRID_Y, 4, gh), WHITE, 160, visible=0)
    # ------------------------------------------------------------ the two models: logos + the current-faction check (3D models: t2r-ext)
    for k, m, alpha in (("F", FRONT, 255), ("B", BACK, BACK_A)):
        h = m["logo_h"]
        b.image(f"logo{k}", a, b.ctr(m["cx"], m["logo_cy"], h, h), LOGO["wlf" if k == "F" else "scar"], WHITE, alpha,
                scaleMode="StretchAspect")
        d = 64 if k == "F" else 44
        b.image(f"chk{k}", a, b.ctr(m["cx"] + h * 0.42, m["logo_cy"] + h * 0.42, d, d), ICONS + "cz-check.png", WHITE, 255,
                visible=0)
        hx, hy, hw, hh = m["hit"]
        b.rect(f"czHit{k}", a, b.tl(hx, hy, hw, hh), WHITE, HIT_A)
    b.text("mdlName", a, b.tl(FRONT["cx"] - 700, FRONT["logo_cy"] + FRONT["logo_h"] / 2 + 20, 1400, 70), "WLF", 40, 0xFFE6E6E6,
           align=1, tracking=6)
    # ------------------------------------------------------------ prompts bottom right (the stock RANDOM / SELECT / BACK row)
    for nm, gl, tx, gx, lx, w in prompt_boxes():
        b.image(nm + "Ic", a, b.ctr(gx + GLYPH / 2, PROMPT_CY, GLYPH, GLYPH), gl, WHITE)
        b.text(nm + "Tx", a, tlm(lx, PROMPT_CY - 40, w + 40, 80), tx, PROMPT_PT, PROMPT, tracking=2)
        b.rect(nm + "Hit", a, b.tl(gx - 16, PROMPT_CY - 55, lx + w - gx + 32, 110), WHITE, HIT_A)
    # CHALLENGES (customize_challenges.c): no menu command is free on this page, so the key is drawn as text in a white box
    # (C on the keyboard, X on the pad; customize.c writes the key and the label), left of SWITCH FACTION
    gx = prompt_boxes()[-1][3] - 80
    lw = CH_W
    kx = gx - lw - 16 - CH_KEY
    b.rect("pChKb", a, b.tl(kx, PROMPT_CY - CH_KEY / 2, CH_KEY, CH_KEY), 0xFFE6E6E6, 255)
    b.text("pChKt", a, b.tl(kx, PROMPT_CY - CH_KEY / 2, CH_KEY, CH_KEY), "C", 34, 0xFF141414, align=1, tracking=1)
    b.text("pChTx", a, tlm(gx - lw, PROMPT_CY - 40, lw + 40, 80), "CHALLENGES", PROMPT_PT, PROMPT, tracking=2)
    b.rect("pChHit", a, b.tl(kx - 16, PROMPT_CY - 55, gx - kx + 32, 110), WHITE, HIT_A)
    # ZOOM (customize.c g_cz_zoom): Z on the keyboard, R3 on the pad (the key box is wide enough for "R3"), left of CHALLENGES
    zgx = kx - 80
    zkx = zgx - ZM_W - 16 - ZM_KEY
    b.rect("pZmKb", a, b.tl(zkx, PROMPT_CY - CH_KEY / 2, ZM_KEY, CH_KEY), 0xFFE6E6E6, 255)
    b.text("pZmKt", a, b.tl(zkx, PROMPT_CY - CH_KEY / 2, ZM_KEY, CH_KEY), "Z", 34, 0xFF141414, align=1, tracking=1)
    b.text("pZmTx", a, tlm(zgx - ZM_W, PROMPT_CY - 40, ZM_W + 40, 80), "ZOOM IN", PROMPT_PT, PROMPT, tracking=2)
    b.rect("pZmHit", a, b.tl(zkx - 16, PROMPT_CY - 55, zgx - zkx + 32, 110), WHITE, HIT_A)
    # a refusal / note, bottom left (its box ends before the ZOOM prompt)
    b.text("pHint", a, tlm(GRID_X, PROMPT_CY - 40, min(1500, zkx - 60 - GRID_X), 80), "", 40, DIM, tracking=2)
    # top right, under the header rule: the outfit being edited is the other faction's (customize.c writes it; empty otherwise)
    # (more visible, on a rectangle frame with dark in it: the note stands on a dark box with a thin
    # white frame, as wide as its text - customize.c stretches the fill and the two long rules from the left (FAC_W is the width they
    # are laid out at) and moves the left edge; the whole group is hidden while there is no note)
    fb = b.group("facBox", a, b.tl(0, 0, 3840, 2160))["guid"]
    fx0 = RIGHT_X - FAC_W
    b.rect("facBg", fb, b.tl(fx0, FAC_Y, FAC_W, FAC_H), 0xFF000000, 190)
    b.rect("facFrT", fb, b.tl(fx0, FAC_Y, FAC_W, 2), WHITE, FAC_LINE_A)
    b.rect("facFrB", fb, b.tl(fx0, FAC_Y + FAC_H - 2, FAC_W, 2), WHITE, FAC_LINE_A)
    b.rect("facFrL", fb, b.tl(fx0, FAC_Y, 2, FAC_H), WHITE, FAC_LINE_A)
    b.rect("facFrR", fb, b.tl(RIGHT_X - 2, FAC_Y, 2, FAC_H), WHITE, FAC_LINE_A)
    # (centre the text in its frame: the text's box is centred on the frame's middle and its text is centred
    # in it - customize.c moves the pivot to the middle of the fitted frame, so an estimate of the text's width that is a little
    # off only changes the padding, on both sides alike)
    b.text("facNote", fb, b.ctr(RIGHT_X - FAC_W / 2.0, FAC_Y + FAC_H / 2.0, 1700, FAC_H), "", 30, 0xFFE6E6E6, align=1, tracking=2)
    build_req(b, a)
    # the first-visit CHALLENGES tip (hidden; customize.c opens it once): customize_challenges_widgets.build_tip
    import customize_challenges_widgets
    customize_challenges_widgets.build_tip(b, a)
    # preload strip: every icon the page may show, tiny + always visible (textures stream in before they are needed; gearup13 pattern)
    pre = b.group("pre", a, b.tl(0, 0, 3, 3))["guid"]
    # (not the card pictures: 12 cards + the strip + the page stay under gui2's 32 images at once; nor the four MELEE STYLE / SEX
    # card icons, two cards each)
    pics = [ICONS + f"cz-{n}.png" for n in ("hat", "torso", "lower", "backpack", "face", "melee", "mask", "sex", "check", "lock")] + list(LOGO.values())
    for i, p in enumerate(pics):
        b.image(f"pre{i}", pre, b.tl(0, 0, 3, 3), p, WHITE, 255)
    w = {k: copy.deepcopy(v) for k, v in main.items() if k != "items"}
    w["items"] = b.items
    return w


def add_customize_page(wdir):
    """to1/customize + to1/customize-challenges (customize_challenges_widgets.py) -> how many pages were written."""
    import customize_challenges_widgets as CCW
    main = json.load(open(os.path.join(wdir, "to1", "main-menu.widget.json"), encoding="utf-8"))
    json.dump(build_page(main), open(os.path.join(wdir, "to1", PAGE + ".widget.json"), "w", encoding="utf-8"), indent=4)
    json.dump(CCW.build_page(main), open(os.path.join(wdir, "to1", CCW.PAGE + ".widget.json"), "w", encoding="utf-8"), indent=4)
    return 2


def add_customize_row(wdir):
    """The CUSTOMIZE row right under CLAN on the MP main menu and the find-match menu (modepick_widgets.add_clan_row's copy).
    Call after modepick_widgets.add_modepick_ui (the Clan rows must exist)."""
    import modepick_widgets
    n = 0
    for page in ("main-menu", "find-match-menu"):
        p = os.path.join(wdir, "to1", page + ".widget.json")
        w = json.load(open(p, encoding="utf-8"))
        _, rows = modepick_widgets._list_rows(w)
        if not any(r["name"] == "Clan" for r in rows):
            continue
        c = modepick_widgets.add_clan_row(w, "Clan")
        c.update(name="Customize", text="CUSTOMIZE")
        json.dump(w, open(p, "w", encoding="utf-8"), indent=4)
        n += 1
    return n


# ------------------------------------------------------------------ offline previews (filled the way customize.c fills the page)
def progress(u, prog):
    return prog.get(("mapwins", u[2]), 0) if u[0] == "mapwins" else prog.get(u[0], 0)


def locked(it, prog):
    u = it.get("unlock")
    return bool(u) and progress(u, prog) < u[1]


def card_label(s, n=13):
    """A card name longer than n characters breaks at the space nearest its middle (customize.c cz_card_label: the same rule by
    the drawn width)."""
    if len(s) <= n or not (" " in s or "-" in s):
        return s
    sp = [k for k, ch in enumerate(s) if ch in " -"]                 # (a hyphen stays, the line ends after it)
    k = min(sp, key=lambda k: abs(k - len(s) / 2))
    return s[:k] + chr(10) + s[k + 1:] if s[k] == " " else s[:k + 1] + chr(10) + s[k + 1:]


def demo(page, fac="wlf", sex="m", cat=1, foc=4, eq=10, off=0, cur_fac="wlf", prog=None):
    """prog = unlock progress {challenge key: n, ("mapwins", map index): n} (customize.c reads t2r-customize.txt)."""
    prog = prog or {}
    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import customize_items as CI
    w = copy.deepcopy(page)
    by = {i["name"]: i for i in w["items"]}
    T = CI.table()[(fac, sex)]
    c = CI.CATS[cat]
    items = T[c]
    foc, eq = min(foc, len(items) - 1), min(eq, len(items) - 1)       # (a preview view written for a longer list)
    for i in range(len(CI.CATS)):
        by[f"tabIc{i}"]["color.a"] = 255 if i == cat else TAB_A_OFF
    by["titleSub"]["text"] = CI.CAT_LABEL[c]
    rx = HEAD_SUB_X + len(CI.CAT_LABEL[c]) * 56 + 65                   # (t2r-ext measures; the preview estimates)
    by["titleRule"]["pivotAs.x"], by["titleRule"]["rectSizeDelta.width"] = rx, RIGHT_X - rx
    by["secName"]["text"] = items[foc]["label"]
    x0 = ITEM_X + len(items[foc]["label"]) * 36 + 60                  # (t2r-ext measures the name; the preview estimates)
    by["secRule"]["pivotAs.x"] = x0
    by["secRule"]["rectSizeDelta.width"] = ITEM_RULE_X1 - x0
    for i in range(NCARD):
        k = off + i
        g = by[f"cd{i}"]
        if k >= len(items):
            g["visible"] = 0
            continue
        it = items[k]
        ic = {"melee": ("cz-fists", "cz-knife"), "sex": ("cz-male", "cz-female")}.get(c)
        by[f"cdIc{i}"]["path"] = ICONS + (ic[k] if ic else f"cz-{c}") + ".png"
        pic = CI.card_pic(c, it)
        by[f"cdPic{i}"]["visible"], by[f"cdIc{i}"]["visible"] = int(bool(pic)), int(not pic)
        if pic:
            by[f"cdPic{i}"]["path"] = ICONS + pic + ".png"
        by[f"cdNm{i}"]["text"] = card_label(it["label"])
        f = k == foc
        by[f"cdFill{i}"]["visible"] = by[f"cdTop{i}"]["visible"] = by[f"cdBot{i}"]["visible"] = int(f)
        by[f"cdIc{i}"]["color.a"] = 255 if f else 130
        by[f"cdPic{i}"]["color.a"] = 255 if f else 180
        by[f"cdNm{i}"]["tint.start"] = by[f"cdNm{i}"]["tint.end"] = 0xFFE6E6E6 if f else DIM
        by[f"cdEq{i}"]["visible"] = int(k == eq)
        by[f"cdLock{i}"]["visible"] = int(locked(it, prog))
    if len(items) > NCARD:
        by["sbTrack"]["visible"] = by["sbThumb"]["visible"] = 1
        gh = ROWS * CARD_H + (ROWS - 1) * GAP
        th = gh * NCARD / len(items)
        by["sbThumb"]["rectSizeDelta.height"] = th
        by["sbThumb"]["pivotAs.y"] = GRID_Y + (gh - th) * off / max(1, len(items) - NCARD)
    other = "scar" if fac == "wlf" else "wlf"
    by["logoF"]["path"], by["logoB"]["path"] = LOGO[fac], LOGO[other]
    by["chkF"]["visible"], by["chkB"]["visible"] = int(cur_fac == fac), int(cur_fac == other)
    for n in ("facBg", "facFrT", "facFrB", "facFrL", "facFrR", "facNote"):          # the other-faction note (customize.c fits the box to the text)
        by[n]["visible"] = int(cur_fac != fac)
    by["facNote"]["text"] = "NOT YOUR CURRENT FACTION - WORN ONCE YOU SWITCH SIDES" if cur_fac != fac else ""
    by["mdlName"]["text"] = {"wlf": "WLF", "scar": "SERAPHITES"}[fac]
    fu = items[foc].get("unlock")
    by["pHint"]["text"] = ""
    if fu:
        req_demo(by, CI.unlock_title(fu), CI.unlock_text(fu), progress(fu, prog), fu[1], not locked(items[foc], prog))
    return w


def stage_bg(fac, out_png):
    """Preview backdrop: a black model-viewer stage with a soft spotlight pool on the front model and the faction figure art as
    STAND-INS for the live 3D models (re/figures fig-*.png; in game they are the real characters, see the doc)."""
    from PIL import Image, ImageDraw, ImageFilter, ImageEnhance
    W, H = 1920, 1080
    im = Image.new("RGBA", (W, H), (9, 9, 10, 255))
    glow = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(glow)
    d.ellipse([FRONT["cx"] / 2 - 300, 120, FRONT["cx"] / 2 + 300, 760], fill=60)        # the spotlight cone ...
    d.ellipse([FRONT["cx"] / 2 - 260, 690, FRONT["cx"] / 2 + 260, 770], fill=120)       # ... and its pool on the floor
    glow = glow.filter(ImageFilter.GaussianBlur(60))
    im.alpha_composite(Image.merge("RGBA", (glow, glow, glow, glow.point(lambda v: v))), (0, 0))
    figs = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "re", "figures")
    if not os.path.isdir(figs):
        figs = os.path.join(".", "re", "figures")
    key = {"wlf": "fig-wlf.png", "scar": "fig-ser.png"}
    other = "scar" if fac == "wlf" else "wlf"
    for f, m, hgt, dim in ((other, BACK, 400, True), (fac, FRONT, 600, False)):
        fig = Image.open(os.path.join(figs, key[f])).convert("RGBA")
        fig = fig.resize((int(fig.width * hgt / fig.height), hgt), Image.LANCZOS)
        if dim:
            fig = ImageEnhance.Brightness(fig).enhance(0.28)
        else:
            fig = ImageEnhance.Brightness(fig).enhance(0.92)
        foot = (m["logo_cy"] - m["logo_h"] / 2) / 2 - 30
        im.alpha_composite(fig, (int(m["cx"] / 2 - fig.width / 2), int(foot - hgt)))
    im.convert("RGB").save(out_png)
    return out_png


def preview(wdir, out_dir):
    import sys
    import tempfile
    import widget_preview
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "models"))
    import real_icons
    import customize_icons
    main = json.load(open(os.path.join(wdir, "to1", "main-menu.widget.json"), encoding="utf-8"))
    page = build_page(main)
    os.makedirs(out_dir, exist_ok=True)
    icd = os.path.join(tempfile.gettempdir(), "cz-icons")
    os.makedirs(icd, exist_ok=True)
    for n, fn in customize_icons.ICONS.items():
        fn().save(os.path.join(icd, n + ".png"))
    real_icons.MOD_DIRS.append(customize_icons.CARDS)
    real_icons.MOD_DIRS.append(icd)
    logos = os.environ.get("T2R_CZ_LOGOS", "../wt/cz-scratch/logos")       # core.psarc gtex1/streaming/photo-mode/logos (extracted)
    widget_preview.STREAMING["streaming/photo-mode/logos/"] = logos
    mid = {"weeks": 6, "rank": 1, "survivors": 23, "revives": 31, "safes": 2, ("mapwins", 1): 4, ("mapwins", 0): 7, "downs": 140, "executions": 38,
           "headshots": 12, "marks": 61, "streak": 5, "assists": 27, "downs-9mm": 22, "melee-downs": 9, "gifts": 4, "heals": 11}   # a player mid first journey
    views = [("wlf-torso", dict(fac="wlf", cat=1, foc=4, eq=10, prog=mid)),
             ("wlf-headwear", dict(fac="wlf", cat=0, foc=2, eq=0, prog=mid)),
             ("wlf-headwear-locked", dict(fac="wlf", cat=0, foc=4, eq=2, prog=mid)),
             ("wlf-lower", dict(fac="wlf", cat=2, foc=3, eq=3)),
             ("wlf-face-scrolled", dict(fac="wlf", cat=4, foc=13, eq=10, off=6)),
             ("scar-torso", dict(fac="scar", cat=1, foc=1, eq=0, cur_fac="wlf", prog=mid)),
             ("scar-torso-scrolled", dict(fac="scar", cat=1, foc=14, eq=0, off=9, cur_fac="wlf", prog=mid)),
             ("scar-female-hat", dict(fac="scar", sex="f", cat=0, foc=1, eq=0, cur_fac="wlf")),
             ("wlf-melee", dict(fac="wlf", cat=5, foc=1, eq=0, prog=mid)),
             ("wlf-mask", dict(fac="wlf", cat=6, foc=3, eq=0, prog=mid)),
             ("wlf-sex", dict(fac="wlf", cat=7, foc=0, eq=0))]
    out = []
    for name, kw in views:
        tmp = os.path.join(tempfile.gettempdir(), f"t2r-customize-{name}.widget.json")
        json.dump(demo(page, **kw), open(tmp, "w", encoding="utf-8"))
        bg = stage_bg(kw["fac"], os.path.join(tempfile.gettempdir(), f"cz-bg-{kw['fac']}.png"))
        out.append(widget_preview.render(tmp, os.path.join(out_dir, f"customize-{name}.png"), bg=bg))
    return out


if __name__ == "__main__":
    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    for p in preview(sys.argv[1], sys.argv[2]):
        print(p)
