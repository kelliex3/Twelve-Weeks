"""2013 match flow widgets (2013matchflow/*.png), added to gui2-widgets.pak by widgets_patch.patch_widgets.

to1/matchflow (new page, pushed by t2r-ext feature_matchflow while the server's start countdown runs,
T2R_MAP_VOTE=1), one page, two stages (groups) over the same dark backdrop:
  "vote"     FIND MATCH / SUPPLY RAID: "Found game, waiting for players..." until the server's vote state
             is in, then the map choices (thumbnail, name, one check per vote, the focused one framed),
             the match roster, the clan panel (WEEK n: DAY d ...) on the right.
  "booster"  Choose One-Use Booster - 00:04: Nothing + the boosters (placeholders until boosters exist),
             details of the focused one, over the dimmed map.
LOADING (map name, MISSION, a framed picture and a tip) is not on this page: a menu page is wiped when the
level load starts (the page stack is empty for the whole load - black screen), so it is built into
the engine's own level-load widget loading.widget (build_loading / add_loading_screen), which the engine's
LoadingScreen draws through the load; t2r-ext (ls_*) brings it up at the horn and keeps it until the intro.
CHOOSE LOADOUT between the vote and the boosters is the existing GEAR UP list (t2r-ext lo_choose) with the
red 00:11 timer in its title. t2r-ext fills the texts; python matchflow_widgets.py <extracted widget dir>
<out dir> renders previews (re/factions2013/preview/matchflow-*.png).

Widget space is 3840 x 2160, y down (the 2013 screenshots are 1280 x 720: x3)."""
import copy
import json
import os
import t2guid
import ui_r2_widgets
import rank_badge_widgets as rbw
import nr_style as nr

from clan_widgets import (Builder, _templates, HEALTHY, HUNGRY, SICK, SUPPLY_BLUE, RED_TEXT, SUPPLY_ICON,
                          PERSON_ICON, PM_TEXT, PM_DIM)

PAGE = "matchflow"
VOTE_MAX = 3                        # map choices
# test server (every map in the pool shown at once, each with its own clickable area):
# a vote of more than 3 maps (daemon T2R_VOTE_ALL=1) shows this grid instead of the 3 tiles, with its own roster copy below (t2r-ext mf_vote_fill)
GRID_MAX, GRID_COLS = 20, 5
CHECKS = 8                          # check marks per choice (one per voter)
ROSTER = 8
BOOSTERS = 8                        # Nothing + 7
TIER_ROWS = 3
TEXT = 0xFFE6E6E6
# the real 2013 icons (tools/models/booster_icons.py, from the PS3 net.pak), ids = MF_BOOSTER / tools/clan.py BOOSTERS
BOOSTER_SLUGS = ["nothing", "ammo-start", "ammo-cost", "armor-cost", "power-cost", "short-upgrade", "long-upgrade",
                 "parts-multiplier"]
BOOSTER_ICON = "streaming/t2r/icons/booster-{}.png"
RING = 0xFF5FC9C0                   # the 2013 booster icon ring: teal
CREAM = 0xFFE9E5D2                  # the 2013 loading picture frame
MAPIMG = "streaming/menus/to1/map-pvp-{}.png"

# Rows: 0 Nothing + tools/clan.py BOOSTERS (2013 list: name, what it does). Owned count is 0 for all.
BOOSTER_LIST = [
    ("Nothing", "Earn One-Use Boosters by growing your clan population and completing Missions."),
    ("Starting Ammo Multiplier", "Spawn with 25% more ammo for your firearms. Used up when the match starts."),
    ("Cheaper Ammo", "Ammo costs 25% fewer parts for the whole match. Used up when the match starts."),
    ("Cheaper Armor", "Armor costs 25% fewer parts for the whole match. Used up when the match starts."),
    ("Cheaper Purchased Weapons", "Purchased weapons cost 25% fewer parts for the whole match. Used up when the match starts."),
    ("Starting Small Firearm Upgrade", "Your handgun starts the match upgraded. Not available yet."),
    ("Starting Large Firearm Upgrade", "Your primary starts the match upgraded. Not available yet."),
    ("Increased Part Earnings", "Earn 10% more parts from everything you do. Used up when the match starts."),
]
# Loading tips (2013 tip card: a category and one line), adapted to the WLF / Seraphites war
TIPS = [  # = tools/ext/tips.h T2R_TIPS (the one tip list); TIPS[0] is the loading widget placeholder
    ('WEAPONS', 'The Molotov flushes enemies out of cover. Arc-throw it from low cover to stay hidden while you throw.'),
    ('WEAPONS', 'Shotguns are deadly up close but fall off fast. Keep moving and close the distance.'),
    ('WEAPONS', 'A suppressor keeps your shots quiet. Pick your fights.'),
    ('WEAPONS', 'The bow is silent, and a fully drawn arrow hits harder. Pick your arrows back up after the fight.'),
    ('WEAPONS', 'Bullets drop with gravity. Aim higher to compensate.'),
    ('WEAPONS', "A pipe bomb's fuse burns for a moment before it goes off. Lob one over cover to catch an enemy hiding behind it."),
    ('WEAPONS', 'Smoke bomb clouds completely block enemy vision, hiding your movements and preventing them from firing on you.'),
    ('WEAPONS', 'One-handed melee weapons are fast and durable. Two-handed melee weapons are slower but deal massive damage.'),
    ('WEAPONS', 'Upgraded melee weapons are restored to full durability and have greatly increased damage.'),
    ('SURVIVAL', 'Crafting takes time. Make medkits and bombs before the fight, not during it.'),
    ('SURVIVAL', 'Healing takes time. Run from the fight and use your health kit somewhere safe.'),
    ('SURVIVAL', 'Supply boxes refill over time. Remember where they are.'),
    ('SURVIVAL', 'Armor soaks up damage from every direction. Buy it early.'),
    ('TEAMWORK', "A downed teammate can be revived. Mark them and run to them - the run doesn't drain your stamina."),
    ('TEAMWORK', 'Mark an enemy to show your whole team where they are.'),
    ('TEAMWORK', 'Stick together. A lone survivor is easy prey for the other faction.'),
    ('STEALTH', 'Stay low in the grass. A prone survivor is hard to spot.'),
    ('STEALTH', 'Listen mode shows you footsteps. Moving slowly keeps you off it.'),
    ('STEALTH', "Use the environment to lose your enemies. They will search your last known position, so don't be there."),
    ('STEALTH', 'Breaking glass windows can open up new routes, but the sound will draw enemy attention.'),
    ('COMBAT', 'Enemies are deadly accurate when you charge straight at them. Use the environment to flank them instead.'),
    ('COMBAT', "Enemies are less accurate while you're sprinting away or to the side."),
    ('COMBAT', 'Stay crouched behind cover to protect yourself from gunfire.'),
    ('COMBAT', "Look around to track enemy movements so you aren't caught off guard."),
    ('COMBAT', 'Stand still to steady your aim. Crouching makes you more accurate, and going prone even more.'),
    ('COMBAT', 'Headshots down enemies faster. Take your time and aim.'),
    ('COMBAT', 'Finishing a downed enemy earns more parts. Watch for their friends.'),
    ('COMBAT', "If your usual plan isn't working, vary your tactics: fight quietly, by force, or avoid the fight. Try a new weapon or crafting recipe."),
    ('CLAN', 'Parts you earn in a match become supplies for your clan. Every survivor eats, so keep them fed.'),
]
TIP_PIC = "streaming/t2r/clan/tip-%02d-%d.png"   # = tips_ship.TEX
# Daily mission placeholder (2013: a weapon challenge with three tiers and the survivors they bring)
MISSIONS = [("Shotgun Downs", "Downs with Shotgun"), ("Revolver Downs", "Downs with Revolver"),
            ("Bow Downs", "Downs with Bow"), ("Headshots", "Headshots"), ("Executions", "Executions"),
            ("Revives", "Revives")]
TIERS = [(3, 5), (5, 7), (7, 10)]


# Layout (widget px, 3840 x 2160): the left column LX0..LX1 holds the vote (tiles, roster), the right column RX0..RX1 the clan block
LX0, LX1, RX0, RX1 = 148, 2740, 2940, 3700
VY, TY, TGAP = 420, 520, 36            # the header row of the vote / clan block, tiles' top, gap                  # tiles' top, gap
GCW, GGAP = 440, 24                 # all-maps grid cell width, gap (5 columns = 2296)
GCH = GCW * 9 // 16 + 84            # grid cell height (art 16:9 + the name / votes strip)
GRY0 = 1090                         # the grid mode's roster rows (right column) start
RROW, RCOLW = 84, 1260              # roster row pitch, column width of the 2 x 4 roster under the tiles
VOTE_PROMPTS = 2                    # boxed prompts of the vote: SWITCH, VOTE


def _focus_frame(b, g, name, w, h):
    """nr_style.tile's focus outline (4 white rects), drawn after the picture so it is on top of it."""
    fo = b.group(f"{name}Fo", g, b.fill(), visible=0)["guid"]
    b.rect(f"{name}FoT", fo, b.tl(0, 0, w, 4), nr.WHITE, 255)
    b.rect(f"{name}FoB", fo, b.tl(0, h - 4, w, 4), nr.WHITE, 255)
    b.rect(f"{name}FoL", fo, b.tl(0, 0, 4, h), nr.WHITE, 255)
    b.rect(f"{name}FoR", fo, b.tl(w - 4, 0, 4, h), nr.WHITE, 255)


def _map_cell(b, parent, pre, x, y, w, ih, strip, name_pt, img, grid=0, visible=1):
    """One map choice: a tile (dark fill, 16:9 art, the focused one in a thin white frame, the others' art dimmed), under the art the name
    and `N | M` votes with a thin bar, a YOUR VOTE chip (accent) over the art of the map we voted for. Widgets <pre>Back / Img / Shade / Fo /
    Name / Ct / VbT / VbF / Yv (t2r-ext mfnr.c fills them)."""
    h = ih + strip
    g = b.group(pre, parent, b.tl(x, y, w, h), visible=visible)["guid"]
    b.rect(f"{pre}Back", g, b.fill(), nr.TILE, nr.TILE_A)
    b.image(f"{pre}Img", g, b.tl(0, 0, w, ih), MAPIMG.format(img), 0xFFFFFFFF, 255)
    b.rect(f"{pre}Shade", g, b.tl(0, 0, w, ih), 0xFF000000, 150)
    _focus_frame(b, g, pre, w, h)
    pad = 16 if grid else 24
    ny = ih + (strip * 0.27 if not grid else 29)
    b.text(f"{pre}Name", g, nr.lm(pad, ny - 28, w - 2 * pad - 150, 56), "Forest", name_pt, nr.WHITE, tracking=2 if not grid else 1)
    b.text(f"{pre}Ct", g, nr.lm(w - pad - 150, ny - 28, 150, 56, right=1), "0 | 8", nr.pt(22) if not grid else nr.pt(16), nr.WHITE, align=2, tracking=2)
    nr.bar(b, g, f"{pre}Vb", pad, w - pad, ih + (strip - 34 if not grid else strip - 24), h=6 if not grid else 5)
    gy = b.group(f"{pre}Yv", g, b.tl(16 if not grid else 10, 16 if not grid else 10, 300, 80), visible=0)["guid"]
    nr.chip(b, gy, f"{pre}Yv", 0, 0, "YOUR VOTE", pts=nr.pt(18) if not grid else nr.pt(13), style="acc")
    # Part I maps (img "p1-..."): a small white PART I box top right of the art, like the OG badge on OG customize items (
    # t2r-ext mfnr.c shows it and sizes the box to the text)
    p1pts = nr.pt(18) if not grid else nr.pt(13)
    p1w = nr.est("PART I", p1pts) * 1.06 + 32
    edge = 16 if not grid else 10
    gp = b.group(f"{pre}P1", g, b.tl(0, edge, w, 80), visible=0)["guid"]
    nr.chip(b, gp, f"{pre}P1", w - edge - p1w, 0, "PART I", pts=p1pts, style="white")      # (mfnr.c right-aligns it at w - edge)


def _roster(b, parent, pre, place, row_h=78, pt=36, map_w=440):
    """ROSTER rows <pre>0.. under `parent`; place(r) -> (x, y, width). The vote's roster ('ros', 2 x 4 rows) and the all-maps grid's copy ('gros').
    Thin rule rows: a hairline under each, a team mark (TabB = our team: white bar, TabR = the enemy: grey bar), the rank badge, the name, and at
 the row's end the check + the name of the map the player voted for (name at the start, check + map name at the end; the map name
    is right aligned and t2r-ext mfnr_roster moves the check to just before its text = ck_x(); map_w = mfnr.c MFNR_RMAPW / MFNR_GRMAPW, the name
    box = MFNR_RNAMEW / MFNR_GRNAMEW, the row width = MFNR_RROWW / MFNR_GRROWW)."""
    ck_w, rk_x = 38, 34
    for r in range(ROSTER):
        x, y, w = place(r)
        map_x = w - 16 - map_w
        ck_x = map_x - 16 - ck_w
        g = b.group(f"{pre}{r}", parent, b.tl(x, y, w, row_h), visible=0)["guid"]
        b.rect(f"{pre}{r}Bar", g, b.tl(0, row_h - nr.HAIR, w, nr.HAIR), nr.WHITE, nr.HAIR_A)
        b.rect(f"{pre}{r}TabB", g, b.tl(0, 10, 6, row_h - 22), nr.WHITE, 255, visible=0)
        b.rect(f"{pre}{r}TabR", g, b.tl(0, 10, 6, row_h - 22), nr.GREY, 255, visible=0)
        b.add("Polyline", f"{pre}{r}Ck", g, **b.tl(ck_x, row_h / 2.0 - 17, ck_w, 34), **b.col(nr.WHITE), closed=0, lineWidth=5.0,
              pointsUv=[0.05, 0.55, 0.38, 0.95, 0.97, 0.05], visible=0, **{"color.a": 255})
        rbw.add_to_builder(b, g, f"{pre}{r}Rk", f"{pre}{r}Rn", rk_x, row_h / 2.0, 40, row_h)                     # the rank badge (rank_badge_widgets; t2r-ext mfnr_roster)
        rs = rbw.reserved(40, row_h)
        b.text(f"{pre}{r}Name", g, nr.lm(rk_x + rs, 0, ck_x - 20 - rk_x - rs, row_h), "PLAYER", pt, nr.WHITE, tracking=2)
        b.text(f"{pre}{r}Map", g, nr.lm(map_x, 0, map_w, row_h, right=1), "", nr.pt(20), nr.GREY, align=2, tracking=2)


def ck_x(row_w, map_w, text):
    """= t2r-ext mfnr_roster: the roster check's left edge, just before the right-aligned voted map name (nr_fit shrink, nr_tw width)."""
    pts = nr.pt(20)
    e = nr.est(text, pts)
    s = max(0.6, map_w / e) if e > map_w else 1.0
    return row_w - 16 - e * 1.06 * s - 16 - 38


def build_page(main):
    """to1/matchflow.widget.json content."""
    t, root0, anim0 = _templates(main)
    b = Builder(t)
    root = copy.deepcopy(root0); root.update(guid=t2guid.guid())
    anim = copy.deepcopy(anim0); anim.update(guid=t2guid.guid(), parent=root["guid"])
    b.items += [root, anim]
    a = anim["guid"]
    nr.backdrop(b, a)                  # bgBlur (the engine's live menu blur) + bgDim + bgVig: the vote's frame (t2r-ext mfnr.c hides them in the booster stage)
    b.rect("bgDark", a, b.bleed(), 0xFF0B0C0D, 236, visible=0)                    # the booster stage's own backdrop (shown by t2r-ext)
    b.image("mapBg", a, b.fill(), MAPIMG.format("school"), 0xFFFFFFFF, 110, visible=0)
    b.rect("mapShade", a, b.bleed(), 0xFF000000, 150, visible=0)
    b.image("bgLight", a, b.ctr(1920, 980, 3600, 2200), "hud/player-menu/gradient/radial-gradient.png", 0xFFFFFFFF, 20, visible=0)

    # ------------------------------------------------------------ stage 1: vote (matchfoundwaitingplayers, matchfound2mapvote)
    # No Return frame (re/ui-redesign-1117): breadcrumb FOUND MATCH | MAP VOTE, week block top right, boxed prompts bottom right
    # (nr_style frame parts live under anim-group: t2r-ext mfnr.c shows them for the vote stage only, the booster stage keeps its own look).
    v = b.group("vote", a, b.tl(0, 0, 3840, 2160))["guid"]
    nr.header(b, a, "FOUND MATCH", "MAP VOTE")
    nr.week(b, a, 1, 1, 0)
    nr.prompt_slots(b, a, VOTE_PROMPTS)
    b.text("voteSub", v, nr.lm(nr.L0, nr.TAB_Y - 30, 1200, 60), "SUPPLY RAID", nr.pt(24), nr.GREY, tracking=3)
    b.text("voteWait", v, nr.lm(nr.L0, 440, 2600, 90), "Found game, waiting for players...", nr.pt(28), nr.WHITE, tracking=2)
    # the vote's header row: VOTE FOR A MAP, a hairline, the countdown right (double buffered twin voteTimerB), a thin bar of the time left
    hd = b.group("voteHead", v, b.tl(0, 0, 3840, 2160), visible=0)["guid"]
    nr.section(b, hd, "vtMap", LX0, LX1 - 250, VY, "VOTE FOR A MAP", "", pts=nr.pt(24))
    nr.bar(b, hd, "vtTm", LX0, LX1, VY + 42)
    for nm, vis in (("voteTimer", 1), ("voteTimerB", 0)):
        b.text(nm, v, nr.lm(LX1 - 240, VY - 30, 240, 60, right=1), "00:15", nr.pt(26), nr.WHITE, align=2, tracking=3, visible=vis)
    # the three map tiles (big, 16:9 art, name + N | M votes + a thin bar under it)
    tiles = b.group("voteTiles", v, b.tl(0, 0, 3840, 2160), visible=0)["guid"]
    TW = (LX1 - LX0 - 2 * TGAP) // 3
    for k in range(VOTE_MAX):
        _map_cell(b, tiles, f"tile{k}", LX0 + k * (TW + TGAP), TY, TW, TW * 9 // 16, 130, nr.pt(26), "forest")
    # the all-maps grid (test server): GRID_COLS columns, one cell per map, the roster in the right column
    grid = b.group("voteGrid", v, b.tl(0, 0, 3840, 2160), visible=0)["guid"]
    for k in range(GRID_MAX):
        _map_cell(b, grid, f"g{k}", LX0 + (k % GRID_COLS) * (GCW + GGAP), TY + (k // GRID_COLS) * (GCH + 20), GCW, GCW * 9 // 16, 84, nr.pt(18), "forest",
                  grid=1, visible=0)
    # the roster: our team left, the enemy right (two columns of four) under the tiles; in the grid mode one column of eight in the right column
    rows = b.group("voteRoster", v, b.tl(0, 0, 3840, 2160), visible=0)["guid"]
    nr.section(b, rows, "vtPl", LX0, LX1, 1210, "PLAYERS", "0 | 8 VOTED", pts=nr.pt(24))
    _roster(b, rows, "ros", lambda r: (LX0 + (r // 4) * (RCOLW + 72), 1270 + (r % 4) * RROW, RCOLW))
    gg = b.group("voteGridRoster", grid, b.tl(0, 0, 3840, 2160))["guid"]
    nr.section(b, gg, "vtGp", RX0, RX1, GRY0 - 50, "PLAYERS", "0 | 8 VOTED", pts=nr.pt(24))
    _roster(b, gg, "gros", lambda r: (RX0, GRY0 + r * RROW, RX1 - RX0), map_w=250)
    # the clan block, right column: SURVIVORS n, three thin bars (healthy / hungry / sick as N | total), the supplies needed per day
    nr.section(b, v, "vtSv", RX0, RX1, VY, "SURVIVORS", "5", pts=nr.pt(24))
    for k, (key, label) in enumerate((("H", "HEALTHY"), ("U", "HUNGRY"), ("S", "SICK"))):
        y = 510 + k * 96
        b.text(f"vs{key}L", v, nr.lm(RX0, y - 28, 420, 56), label, nr.pt(21), nr.GREY, tracking=3)
        b.text(f"vs{key}N", v, nr.lm(RX1 - 300, y - 28, 300, 56, right=1), "5 | 5", nr.pt(22), nr.WHITE, align=2, tracking=2)
        nr.bar(b, v, f"vs{key}", RX0, RX1, y + 36)
    b.rect("vtNdRl", v, b.tl(RX0, 810, RX1 - RX0, nr.HAIR), nr.WHITE, nr.HAIR_A)
    b.image("voteSupplyIcon", v, b.ctr(RX0 + 30, 870, 56, 56), "streaming/t2r/icons/c13-materials.png", nr.WHITE, 220, scaleMode="StretchAspect")      # (white clan icon: the old supply art is blue)
    b.text("voteNeedL", v, nr.lm(RX0 + 76, 870 - 28, 460, 56), "NEEDED PER DAY", nr.pt(21), nr.GREY, tracking=3)
    b.text("voteNeeded", v, nr.lm(RX1 - 200, 870 - 30, 200, 60, right=1), "9", nr.pt(28), nr.WHITE, align=2, tracking=2)

    # ------------------------------------------------------------ stage 3: one-use booster (matchfound4boosterselectnoloadyet)
    bo = b.group("booster", a, b.tl(0, 0, 3840, 2160), visible=0)["guid"]
    b.text("boTitle", bo, b.tl(486, 150, 2900, 150), 'Choose One-Use Booster - <font color="#e04848">00:05</font>', 84,
           0xFFD8D4CC, tracking=4)
    # double buffer (timer flicker): the ext writes the hidden copy, then swaps
    b.text("boTitleB", bo, b.tl(486, 150, 2900, 150), 'Choose One-Use Booster - <font color="#e04848">00:05</font>', 84,
           0xFFD8D4CC, tracking=4, visible=0)
    for k in range(BOOSTERS):
        cy = 470 if k == 0 else 609 + (k - 1) * 130
        g = b.group(f"boRow{k}", bo, b.tl(0, 0, 3840, 2160))["guid"]
        b.rect(f"boSel{k}", g, b.tl(500, cy - 58, 1330, 116), 0xFFFFFFFF, 34, visible=0)
        d = 80 if k else 90
        b.image(f"boIcon{k}", g, b.ctr(610, cy, d, d), BOOSTER_ICON.format(BOOSTER_SLUGS[k]), 0xFFFFFFFF, 255)
        name, _ = BOOSTER_LIST[k]
        b.text(f"boName{k}", g, b.tl(710, cy - 45, 1000, 90), name, 48, TEXT if k == 0 else 0xFF8E8A84, tracking=2)
        if k:
            b.text(f"boCount{k}", g, b.tl(1500, cy - 45, 300, 90), "x0", 48, 0xFF8E8A84, align=2, tracking=2)
    b.line("boDivider", bo, 1872, 430, 1872, 1690, 0xFFFFFFFF, 160, 2.0)
    b.image("boBig", bo, b.ctr(2115, 665, 170, 170), BOOSTER_ICON.format(BOOSTER_SLUGS[0]), 0xFFFFFFFF, 255)
    b.text("boBigName", bo, b.tl(2265, 600, 1500, 130), "Nothing", 64, TEXT, tracking=3)
    b.text("boDesc", bo, b.tl(1965, 1240, 1560, 300), BOOSTER_LIST[0][1], 46, TEXT, multiline=1, valign=0)
    b.image("boSelIcon", bo, b.tl(270, 1898, 56, 56), "menu:select", 0xFFFFFFFF)
    b.text("boSelText", bo, b.tl(350, 1890, 1900, 70), "CONFIRM", 46, PM_TEXT, tracking=4)
    b.text("boHint", bo, b.tl(2700, 1890, 900, 70), "W / S  TO SWITCH", 38, PM_DIM, tracking=4)
    ph = b.group("boHintPad", bo, b.tl(2700, 1890, 900, 70), visible=0)["guid"]
    b.image("boHintPrev", ph, b.tl(0, 7, 56, 56), "menu:prev", 0xFFFFFFFF)
    b.image("boHintNext", ph, b.tl(70, 7, 56, 56), "menu:next", 0xFFFFFFFF)
    b.text("boHintPadTx", ph, b.tl(150, 0, 750, 70), "TO SWITCH", 38, PM_DIM, tracking=4)

    # (stage 3, LOADING, is the engine's own loading screen re-skinned: build_loading / add_loading_screen)

    # (the UI round 2 vote restyle, ui_r2_widgets.vote, is gone: the No Return frame replaced the vote's widgets)
    w = {k: copy.deepcopy(v) for k, v in main.items() if k != "items"}
    w["items"] = b.items
    return w


def build_loading(b, parent):
    """The 2013 LOADING screen (matchfound5loadingandtip): group "t2r" = an opaque backdrop (black, the map
    dimmed), "LOADING", map name, mode, MISSION panel, the framed map picture and a tip. Built into the engine's
    own level-load widget (loading.widget, see add_loading_screen) so it is drawn by the engine's loading screen
    for the whole level load; t2r-ext shows it (visible 0 here: the other loads keep the stock spinner) and fills
    the texts (ls_fill)."""
    g = b.group("t2r", parent, b.tl(0, 0, 3840, 2160), visible=0)["guid"]
    b.rect("ldBlack", g, b.bleed(), 0xFF000000, 255)
    b.image("ldBlur", g, b.bleed(), "general-textures/blur-background.png", 0xFFFFFFFF, 255)
    b.rect("ldDark", g, b.bleed(), 0xFF0B0C0D, 236)
    b.image("mapBg", g, b.fill(), MAPIMG.format("school"), 0xFFFFFFFF, 110)
    b.rect("mapShade", g, b.bleed(), 0xFF000000, 150)
    b.image("ldLight", g, b.ctr(1920, 980, 3600, 2200), "hud/player-menu/gradient/radial-gradient.png", 0xFFFFFFFF, 20)
    ld = b.group("loading", g, b.tl(0, 0, 3840, 2160))["guid"]
    b.text("ldTitle", ld, b.tl(240, 150, 2200, 150), "LOADING", 84, 0xFFD8D4CC, tracking=6)   # x = the separator's start
    b.line("ldLine", ld, 240, 330, 3570, 330, 0xFFFFFFFF, 110, 2.0)
    b.text("ldMap", ld, b.tl(240, 360, 1800, 120), "Forest", 64, TEXT, tracking=3)
    b.text("ldMode", ld, b.tl(240, 470, 1800, 80), "SUPPLY RAID", 46, PM_DIM, tracking=4)
    b.text("ldSpin", ld, b.tl(2260, 1890, 1240, 80), "LOADING", 46, PM_DIM, align=2, tracking=6)
    # double buffer (timer flicker): the ext writes the hidden copy, then swaps
    b.text("ldSpinB", ld, b.tl(2260, 1890, 1240, 80), "LOADING", 46, PM_DIM, align=2, tracking=6, visible=0)
    b.text("ldMissionHead", ld, b.tl(260, 850, 900, 90), "MISSION:", 54, TEXT, tracking=5)
    b.line("ldMisLine1", ld, 255, 940, 1165, 940, 0xFFFFFFFF, 130, 2.0)
    b.text("ldMis1", ld, b.tl(450, 960, 800, 80), "", 42, TEXT, tracking=2)   # The placeholder showed when the ext had not filled it yet - empty until filled
    b.text("ldMis2", ld, b.tl(450, 1040, 800, 80), "", 42, TEXT, tracking=2)
    b.line("ldMisLine2", ld, 255, 1135, 1165, 1135, 0xFFFFFFFF, 130, 2.0)
    for k in range(TIER_ROWS):
        y = 1170 + k * 86
        b.text(f"ldTier{k}", ld, b.tl(260, y, 600, 80), "", 40, PM_DIM, tracking=2)
        b.image(f"ldTier{k}Icon", ld, b.tl(870, y + 6, 28, 62), PERSON_ICON, PM_DIM)
        b.text(f"ldTier{k}Pct", ld, b.tl(920, y, 240, 80), "", 40, PM_DIM, tracking=2)
    b.line("ldDivider", ld, 1326, 545, 1326, 1850, 0xFFFFFFFF, 150, 2.0)
    b.rect("ldPicBack", ld, b.tl(1614, 606, 1548, 870), 0xFF050606, 255)
    # the tip's own screenshot (as 2013: streaming/t2r/clan/tip-NN-k.png, tools/models/figures/tips_ship.py). ldPicOld under it
    # keeps the previous tip's picture while the new one streams in (no empty box at the 7 s change); the ext fills both
    b.image("ldPicOld", ld, b.tl(1620, 612, 1536, 858), TIP_PIC % (1, 1), 0xFFFFFFFF, 255)
    b.image("ldPic", ld, b.tl(1620, 612, 1536, 858), TIP_PIC % (1, 1), 0xFFFFFFFF, 255)
    b.image("ldPicShade", ld, b.tl(1620, 612, 1000, 260), "streaming/t2r/clan/tip-cat-shade.png", 0xFFFFFFFF, 255)   # tips_ship.make_cat_shade
    b.frame("ldPicFrame", ld, b.tl(1614, 606, 1548, 870), CREAM, 255, 7.0)
    b.text("ldTipCat", ld, b.tl(1650, 630, 1000, 100), "WEAPONS:", 54, TEXT, tracking=4)
    b.text("ldTip", ld, b.tl(1614, 1530, 1548, 260), TIPS[0][1], 54, TEXT, align=1, multiline=1, valign=0)
    # hidden twins (the tips flickered when they changed) - db_text_w swaps them once built
    b.text("ldTipCatB", ld, b.tl(1650, 630, 1000, 100), "WEAPONS:", 54, TEXT, tracking=4, visible=0)
    b.text("ldTipB", ld, b.tl(1614, 1530, 1548, 260), TIPS[0][1], 54, TEXT, align=1, multiline=1, valign=0, visible=0)
    if ui_r2_widgets.on("load"):        # UI round 2 (A2)
        ui_r2_widgets.loading(b, g)
    return g


LOADING = "loading"                 # the engine's level-load widget (LoadingScreen::SetEnabled spawns it)


def add_loading_screen(wdir):
    """loading.widget.json: the stock widget (root > fader > taskProgressBar spinner + loadText) gets the 2013
    LOADING group "t2r" as root's first child (drawn under the fader; t2r-ext hides the fader while ours shows).
    Groups here are plain gui2 groups (no Menu2 engine class: this widget is not a menu page)."""
    path = os.path.join(wdir, LOADING + ".widget.json")
    w = json.load(open(path, encoding="utf-8"))
    if any(i["name"] == "t2r" for i in w["items"]):
        return 0
    main = json.load(open(os.path.join(wdir, "to1", "main-menu.widget.json"), encoding="utf-8"))
    t, _, _ = _templates(main)
    for c in t.values():
        for k in ("engineClass", "tags", "animationsPath"):
            c.pop(k, None)
    b = Builder(t)
    root = next(i for i in w["items"] if i["parent"] is None)
    build_loading(b, root["guid"])
    # "t2r" is the first item of the build: depth 0.0, under the fader (depth 1.0)
    at = next(k for k, i in enumerate(w["items"]) if i["name"] == "fader")
    w["items"][at:at] = b.items
    json.dump(w, open(path, "w", encoding="utf-8"), indent=4)
    return len(b.items)


def add_matchflow_page(wdir):
    """Edit the extracted widget folder: write to1/matchflow.widget.json (the main menu is the template)."""
    main = json.load(open(os.path.join(wdir, "to1", "main-menu.widget.json"), encoding="utf-8"))
    json.dump(build_page(main), open(os.path.join(wdir, "to1", PAGE + ".widget.json"), "w", encoding="utf-8"), indent=4)
    return 1


LOBBY_MAP = ("mapGroup", "mapImage", "mapFrame", "mapNameBg", "mapName")   # find-match-menu's map preview block


def hide_lobby_map(wdir):
    """MATCH FOUND (to1/find-match-menu): the lobby's selected-map preview bottom left (anim-group.mapGroup:
    thumbnail, outline, name band, map name) is gone - the map is decided by the vote only.
    winmm flips the group visible (and fills mapImage / mapName) when a match is found, so the data hides
    the whole block: every child visible 0 and the group's alpha 0 (its children inherit it)."""
    path = os.path.join(wdir, "to1", "find-match-menu.widget.json")
    w = json.load(open(path, encoding="utf-8"))
    n = 0
    for i in w["items"]:
        if i["name"] in LOBBY_MAP:
            i["visible"] = 0
            if i["name"] == "mapGroup":
                i["color.a"] = 0
            n += 1
    json.dump(w, open(path, "w", encoding="utf-8"), indent=4)
    return n


def twin_status_label(wdir):
    """MATCH FOUND / STARTING MATCH IN n... (to1/find-match-menu anim-group.statusLabel): winmm rewrites it every second and a gui2
    text whose string changes draws nothing for a frame (the countdown flickers). A hidden twin statusLabelB (same
    parent / layout) - the ext's settext hook writes the hidden one and swaps them once it is built (db_text_w)."""
    import t2guid
    path = os.path.join(wdir, "to1", "find-match-menu.widget.json")
    w = json.load(open(path, encoding="utf-8"))
    items = w["items"]
    if any(i["name"] == "statusLabelB" for i in items):
        return 0
    for k, i in enumerate(items):
        if i["name"] == "statusLabel":
            t = copy.deepcopy(i)
            t["name"] = "statusLabelB"
            t["guid"] = t2guid.guid()
            t["visible"] = 0
            items.insert(k + 1, t)
            json.dump(w, open(path, "w", encoding="utf-8"), indent=4)
            return 1
    return 0


def twin_score_banner(wdir):
    """The in-match top score banner (to1/score-banner, teamPanel): winmm's updater rewrites matchTimer / allyLives / enemyLives
    through settext on every call and a gui2 text whose string changes draws nothing for a frame (the timer flickered once a
    second). Hidden twins matchTimerB / allyLivesB / enemyLivesB (same parent / layout): the ext's settext hook writes the hidden
    one on change only and swaps them once it is built (db_text_wd, DB_BANNER)."""
    path = os.path.join(wdir, "to1", "score-banner.widget.json")
    w = json.load(open(path, encoding="utf-8"))
    items = w["items"]
    have = {i["name"] for i in items}
    n = 0
    for name in ("matchTimer", "allyLives", "enemyLives"):
        if name + "B" in have:
            continue
        for k, i in enumerate(items):
            if i["name"] == name:
                import t2guid
                t = copy.deepcopy(i)
                t["name"] = name + "B"
                t["guid"] = t2guid.guid()
                t["visible"] = 0
                items.insert(k + 1, t)
                n += 1
                break
    if n:
        json.dump(w, open(path, "w", encoding="utf-8"), indent=4)
    return n


# ------------------------------------------------------------------ 2013 match HUD (timer above the radar, lives under the figures)
# The old top-centre widget (team names, lives, timer, SUPPLY RAID on a dark plate) is CUT: everything of its chrome is hidden
# (the widgets stay in the page - winmm's updater FUN_180063020 still writes them, t2r-ext's settext hook still catches the
# strings). What stays visible is repositioned, 4K page px (1280x720 units x 3), radar disc centre (104, 107) r 75 units:
#   matchTimer (+B)         white, centred above the radar, ~1/3 of the radar's width (2013: "09:31")
#   allyLives / enemyLives  our team (blue) : enemy (red) lives under the team figures; the ":" between them is fixed, each
#                           number is anchored AWAY from the colon (ours right-aligned to its left, theirs left-aligned to its
#                           right) so a size change never reaches the colon or the other number
#   ...Big (+B)             the same strings in the bigger size: t2r-ext shows the Big pair for the team with MORE lives
#   allyUl*/enemyUl*        small team-colour underline under each number (one per size)
HUD_BLUE, HUD_RED = 0xFF48A8D8, 0xFFE04848          # 2013UI.png figure / lives blue (72,168,216), enemy red
# timer: 2013 cap height 26 px of 1079 x K 0.69 = 12 units; the radar's top is at y 32 units, so the text is centred ON the disc's rim
# (3.1 % of the height from the top edge, bottom at the disc's rim at its own width) instead of pressed against the screen edge
# The top-left block moves down 10 % of the screen (216 px) and the timer ends 5 % (108 px) further above the radar -> net +108 px
HUD_SHIFT_PX = 216.0
TIMER_Y, TIMER_PT = 62.0 + HUD_SHIFT_PX / 2, 52
LV_CX, LV_GAP, LV_W = 318.0, 16.0, 300.0           # colon centre (figure row centre), clear gap each side, text box width
LV_Y_BIG, LV_Y_SMALL, LV_Y_MID = 770.0 + HUD_SHIFT_PX, 778.5 + HUD_SHIFT_PX, 775.5 + HUD_SHIFT_PX                # centre lines (small sits lower: same baseline)
# three sizes: LEADER (the team with more lives) 75 pt, EQUAL (tied: both) 60 pt, TRAILER (behind) 52 pt; the centre lines
# differ so the baselines stay level (about 0.3 x the point size difference)
LV_PT_BIG, LV_PT_SMALL, LV_PT_MID = 75, 52, 60
# underline widths per size and digit count (a digit is ~0.52 x the point size wide): 2+ digits / 1 digit, a bit wider than the number
# measured in game (60 pt "20" = 72 px wide, glyph side bearing 6 px at the aligned edge): a digit is ~0.6 x the point size
# wide, and the text starts ~0.1 x the point size off the aligned edge -> the underline is centred on the digits, not on the edge
LV_UL = {"Big2": 90.0, "Big1": 46.0, "Small2": 62.0, "Small1": 32.0}
LV_UL_PAD = {"Big": 7.5, "Small": 5.2}             # side bearing between the aligned edge and the first / last digit
LV_UL_DY = {"Big": 36.0, "Small": 24.0}           # underline centre below the number's centre line


def banner_panel_rects():
    """[(name, cx, cy, w, h)] content boxes (4K px, before the 18 % padding) of the timer and the lives row."""
    return [("pnlTimer", 312.0, TIMER_Y, 150.0, 44.0),
            # sized for the WIDEST case (two big 2-digit numbers, both underlined) and centred on the colon, so whatever the numbers'
            # sizes the panel stays one centred rectangle; top = the big digits' cap line, bottom = the big underline
            ("pnlLives", LV_CX, LV_Y_BIG + 6.0, 2 * (LV_GAP + LV_UL["Big2"] + 8.0), 96.0)]


def remake_score_banner(wdir):
    path = os.path.join(wdir, "to1", "score-banner.widget.json")
    w = json.load(open(path, encoding="utf-8"))
    items = w["items"]
    if any(i["name"] == "allyLivesBig" for i in items):
        return 0
    import t2guid
    by = {i["name"]: i for i in items}
    for nm in ("panelBg", "panelLineTop", "panelLineBottom", "allyLabel", "enemyLabel", "modeLabel", "panelDivider0", "panelDivider1"):
        if nm in by:
            by[nm]["visible"] = 0
    tp = by["teamPanel"]
    tp.update({"anchorMinN.x": 0.0, "anchorMinN.y": 0.0, "anchorMaxN.x": 0.0, "anchorMaxN.y": 0.0, "pivotN.x": 0.0, "pivotN.y": 0.0,
               "pivotAs.x": 0.0, "pivotAs.y": 0.0, "rectSizeDelta.width": 3840.0, "rectSizeDelta.height": 2160.0})

    def place(i, cx, cy, wd, ht, pt=None, argb=None, align=None, vis=None):
        i.update({"anchorMinN.x": 0.0, "anchorMinN.y": 0.0, "anchorMaxN.x": 0.0, "anchorMaxN.y": 0.0, "pivotN.x": 0.5, "pivotN.y": 0.5,
                  "pivotAs.x": cx, "pivotAs.y": cy, "rectSizeDelta.width": wd, "rectSizeDelta.height": ht})
        if pt is not None:
            i["pointSize"] = pt
        if argb is not None:
            i["tint.start"] = i["tint.end"] = argb
        if align is not None:
            i["alignH"] = align
        if vis is not None:
            i["visible"] = vis
        return i

    def clone(src, name, vis):
        c = copy.deepcopy(src)
        c.update(guid=t2guid.guid(), name=name, visible=vis)
        return c
    new = []
    place(by["matchTimer"], 312.0, TIMER_Y, 300.0, 70.0, TIMER_PT, 0xFFE6E6E6)
    if "matchTimerB" in by:
        place(by["matchTimerB"], 312.0, TIMER_Y, 300.0, 70.0, TIMER_PT, 0xFFE6E6E6)
    for side, argb, align, cx in (("ally", HUD_BLUE, 2, LV_CX - LV_GAP - LV_W / 2), ("enemy", HUD_RED, 0, LV_CX + LV_GAP + LV_W / 2)):
        for suf, pt, yy in (("", LV_PT_SMALL, LV_Y_SMALL), ("Mid", LV_PT_MID, LV_Y_MID), ("Big", LV_PT_BIG, LV_Y_BIG)):
            for tw in ("", "B"):
                src = by.get(f"{side}Lives{tw}")
                if src is None:
                    continue
                it = src if suf == "" else clone(src, f"{side}Lives{suf}{tw}", 0)
                place(it, cx, yy, LV_W, 110.0, pt, argb, align, 1 if (suf == "Mid" and not tw) else 0)    # (starts tied: the Mid pair)
                if suf:
                    new.append(it)
    ul = by["panelBg"]
    for side, argb, edge, sgn in (("ally", HUD_BLUE, LV_CX - LV_GAP, -1), ("enemy", HUD_RED, LV_CX + LV_GAP, 1)):
        for suf in ("Big2", "Big1"):                         # (only the leader is underlined)
            wd = LV_UL[suf]
            y = (LV_Y_BIG if suf.startswith("Big") else LV_Y_SMALL) + LV_UL_DY[suf[:-1]]
            c = clone(ul, f"{side}Ul{suf}", 0)      # (t2r-ext shows only the leader's)
            place(c, edge + sgn * (LV_UL_PAD[suf[:-1]] + wd / 2), y, wd, 5.0, argb=argb)
            c["color.a"] = 255
            c["depth"] = 3.0
            new.append(c)
    # soft dark panels (same style as the PARTS / SUPPLIES panel; clan_widgets.add_hud_panels): behind the timer (150 x 40 px text
    # + 18 % padding per side) and behind the lives row (two big 2-digit numbers around the colon + underline), first in teamPanel
    panels = []
    for pname, pcx, pcy, pwd, pht in banner_panel_rects():
        pn = clone(ul, pname, 1)
        pn["path"] = "streaming/t2r/icons/hud-panel-" + pname + ".png"
        import clan_widgets as _cw
        gw, gh = _cw.hud_panel_geom(pname, pwd, pht)[:2]
        place(pn, pcx, pcy, gw, gh, argb=0xFFFFFFFF)
        pn["color.a"] = 255
        pn["depth"] = 0.0
        panels.append(pn)
    colon = clone(by["matchTimer"], "livesColon", 1)
    colon["text"] = ":"
    place(colon, LV_CX, LV_Y_MID - 6.0, 60.0, 110.0, LV_PT_MID, 0xFFD0D0D0, 1)
    new.append(colon)
    at = max(k for k, i in enumerate(items) if i["name"] in ("allyLives", "enemyLives", "allyLivesB", "enemyLivesB")) + 1
    items[at:at] = new
    first = next(k for k, i in enumerate(items) if i["name"] == "panelBg")
    items[first:first] = panels                         # behind everything else in teamPanel
    json.dump(w, open(path, "w", encoding="utf-8"), indent=4)
    return len(new) + len(panels)


def hide_notice_and_version(wdir):
    """No early-access disclaimer above MULTIPLAYER on the boat menu (menu/page-game-select item earlyAccessNotice, "This is a very
    early test build of the multiplayer mod ...") and no version label (anim-group.versionText, bottom left) on the MP menu / pause menu etc.
    Both stay in the files (animation tracks reference them), just invisible with empty text."""
    n = 0
    for rel, names in (("menu/page-game-select", ("earlyAccessNotice",)),
                       ("to1/main-menu", ("versionText",)), ("to1/pause-menu", ("versionText",)),
                       ("to1/find-match-menu", ("versionText",)), ("to1/leaderboard", ("versionText",)),
                       ("to1/options-menu-ingame", ("versionText",))):
        path = os.path.join(wdir, *rel.split("/")) + ".widget.json"
        if not os.path.exists(path):
            continue
        w = json.load(open(path, encoding="utf-8"))
        hit = 0
        for i in w["items"]:
            if i["name"] in names:
                i["visible"] = 0
                if "text" in i:
                    i["text"] = ""
                hit += 1
        if hit:
            json.dump(w, open(path, "w", encoding="utf-8"), indent=4)
            n += hit
    # the boat menu button reads TWELVE WEEKS (literal text: no text-table cipher), every "THE LAST OF US MP" title reads THE LAST OF US TWELVE WEEKS
    path = os.path.join(wdir, "menu", "page-game-select.widget.json")
    w = json.load(open(path, encoding="utf-8"))
    for i in w["items"]:
        if i["name"] == "buttonMultiplayer":
            i["text"] = "TWELVE WEEKS"
            i["cipherId"] = ""
            n += 1
    json.dump(w, open(path, "w", encoding="utf-8"), indent=4)
    for root, _, files in os.walk(wdir):
        for f in files:
            if not f.endswith(".widget.json"):
                continue
            path = os.path.join(root, f)
            raw = open(path, encoding="utf-8").read()
            if "THE LAST OF US MP" in raw:
                open(path, "w", encoding="utf-8").write(raw.replace("THE LAST OF US MP", "THE LAST OF US TWELVE WEEKS"))
                n += 1
    return n


def hide_findmatch_credits(wdir):
    """No CREDITS on the find-match page (to1/find-match-menu menuList: GEAR UP, OPTIONS, CREDITS = the last row); hidden +
    unfocusable, kept in the file for the page's animation tracks."""
    path = os.path.join(wdir, "to1", "find-match-menu.widget.json")
    if not os.path.exists(path):
        return 0
    w = json.load(open(path, encoding="utf-8"))
    n = 0
    for i in w["items"]:
        if i["name"] == "Credits":
            i["visible"] = 0
            i["focusable"] = 0
            i["pivotAs.y"] = -5000.0
            n += 1
    json.dump(w, open(path, "w", encoding="utf-8"), indent=4)
    return n


def rename_factions2(wdir):
    """The boat menu's MULTIPLAYER button reads TWELVE WEEKS, and every "THE LAST OF US MP" text reads "THE LAST OF US TWELVE WEEKS"
    (all widget pages; case kept: upper / title)."""
    import re as _re
    n = 0
    pat = _re.compile(r"(?i)\b(the last of us)\s+(mp|multiplayer)\b")
    for root, _, files in os.walk(wdir):
        for f in files:
            if not f.endswith(".widget.json"):
                continue
            path = os.path.join(root, f)
            w = json.load(open(path, encoding="utf-8"))
            hit = 0
            game_select = f == "page-game-select.widget.json"
            for i in w["items"]:
                t = i.get("text")
                if not isinstance(t, str) or not t:
                    continue
                nt = pat.sub(lambda m: m.group(1) + (" TWELVE WEEKS" if m.group(1).isupper() else " Twelve Weeks"), t)
                if game_select and t.strip().upper() == "MULTIPLAYER":
                    nt = "TWELVE WEEKS" if t.strip().isupper() else "Twelve Weeks"
                if nt != t:
                    i["text"] = nt
                    hit += 1
            if hit:
                json.dump(w, open(path, "w", encoding="utf-8"), indent=4)
                n += hit
    return n


def trim_main_menu(wdir):
    """MP menu (to1/main-menu anim-group.menuList): no MAP SELECTION (useless; the map vote stays), no PATCH NOTES and no CREDITS - all hidden and unfocusable (kept in
    the file: the page's animation tracks reference every item), the entries below them moved up so the list has no gaps."""
    path = os.path.join(wdir, "to1", "main-menu.widget.json")
    w = json.load(open(path, encoding="utf-8"))
    ml = next(i for i in w["items"] if i["name"] == "menuList")
    rows = sorted((i for i in w["items"] if i.get("parent") == ml["guid"]), key=lambda i: i.get("pivotAs.y", 0))
    if not rows:
        return 0
    step = rows[1]["pivotAs.y"] - rows[0]["pivotAs.y"] if len(rows) > 1 else 106.0
    y, n = rows[0]["pivotAs.y"], 0
    for i in rows:
        if i["name"] in ("MapSelect", "Patchnotes", "Credits"):
            i["visible"] = 0
            i["focusable"] = 0
            i["pivotAs.y"] = -5000.0
            n += 1
            continue
        i["pivotAs.y"] = y
        y += step
    json.dump(w, open(path, "w", encoding="utf-8"), indent=4)
    return n


# ------------------------------------------------------------------ offline previews
def _demo_vote(by, maps, votes, focus, mine, roster, me, secs, day, tot=15, pc=None, decided=False):
    """The vote stage as t2r-ext mfnr.c fills it (same strings, same runtime widths through nr_style's demo_* helpers).
    maps [(name, img)], votes [n per map], focus / mine = tile index, roster [(name, voted, team)] team 1 ours / 2 the enemy, me = our row."""
    import nr_style as n
    grid = len(maps) > VOTE_MAX
    pc = pc if pc is not None else len(roster)

    def vis(nm, on):
        by[nm]["visible"] = int(bool(on))

    def txt(nm, t):
        by[nm]["text"] = t
    n.demo_header(by, "FOUND MATCH", "MAP VOTE")
    n.demo_week(by, day // 7 + 1, day % 7 + 1, day)
    prompts = [("W A S D" if grid else "A / D", "SWITCH", 1, None), ("SPACE", "VOTE", int(not decided), "menu:select")]
    n.demo_prompts(by, prompts)
    vis("voteHead", 1); vis("voteTiles", not grid); vis("voteGrid", grid); vis("voteRoster", not grid); vis("voteWait", 0)
    txt("voteSub", "SUPPLY RAID")
    n.demo_section(by, "vtMap", LX0, LX1 - 250, "VOTE FOR A MAP")
    txt("voteTimer", "00:%02d" % secs)
    n.demo_bar(by, "vtTm", LX0, LX1, secs / float(tot))
    cells = ("g", GRID_MAX, GCW, 16) if grid else ("tile", VOTE_MAX, (LX1 - LX0 - 2 * TGAP) // 3, 24)
    for k in range(cells[1]):
        pre = f"{cells[0]}{k}"
        if grid:
            vis(pre, k < len(maps))
        if k >= len(maps):
            continue
        nm, img = maps[k]
        on = decided and k == mine or (not decided and k == focus)
        by[f"{pre}Img"]["path"] = MAPIMG.format(img)
        by[f"{pre}Name"]["text"] = nm
        vis(f"{pre}Fo", on); vis(f"{pre}Shade", not on)
        vis(f"{pre}Yv", k == mine)
        vis(f"{pre}P1", img.startswith("p1-") or "(Part I)" in nm)
        txt(f"{pre}Ct", f"{votes[k]} | {pc}" if votes[k] else f'<font color="#8C8C8C">0 | {pc}</font>')
        by[f"{pre}Ct"]["tint.start"] = by[f"{pre}Ct"]["tint.end"] = n.WHITE if votes[k] else n.GREY
        n.demo_bar(by, f"{pre}Vb", cells[3], cells[2] - cells[3], votes[k] / float(max(1, pc)))
    for pre, wrap in (("ros", not grid), ("gros", grid)):
        for r in range(ROSTER):
            vis(f"{pre}{r}", wrap and r < len(roster))
            if wrap and r < len(roster):
                nm, voted, team = roster[r][:3]
                opt = mine if r == me and mine >= 0 else roster[r][3] if len(roster[r]) > 3 else 0 if voted else -1    # the option voted for (mfnr_roster: po; our row = my vote)
                voted = voted or (r == me and mine >= 0)
                col = n.ACC if r == me else n.GREY if team == 2 else n.WHITE
                by[f"{pre}{r}Name"]["tint.start"] = by[f"{pre}{r}Name"]["tint.end"] = col
                txt(f"{pre}{r}Name", nm)
                vis(f"{pre}{r}Ck", voted); vis(f"{pre}{r}TabB", team == 1); vis(f"{pre}{r}TabR", team == 2)
                mtxt = maps[opt][0] if voted and 0 <= opt < len(maps) else ""
                txt(f"{pre}{r}Map", mtxt)
                if mtxt:
                    by[f"{pre}{r}Ck"]["pivotAs.x"] = ck_x(RCOLW if pre == "ros" else RX1 - RX0, 440 if pre == "ros" else 250, mtxt)
                vis(f"{pre}{r}Rk", 1); txt(f"{pre}{r}Rn", str((7 * r + 3) % 40 + 1)); vis(f"{pre}{r}Rn", 1)
                by[f"{pre}{r}Map"]["tint.start"] = by[f"{pre}{r}Map"]["tint.end"] = n.ACC if r == me else n.GREY
    nv = sum(1 for r in roster if r[1])
    if grid:
        n.demo_section(by, "vtGp", RX0, RX1, "PLAYERS", f"{nv} | {pc} VOTED")
    else:
        n.demo_section(by, "vtPl", LX0, LX1, "PLAYERS", f"{nv} | {pc} VOTED")
    sv = {"H": 25, "U": 0, "S": 0}
    tot_sv = sum(sv.values())
    n.demo_section(by, "vtSv", RX0, RX1, "SURVIVORS", str(tot_sv))
    for key, c in sv.items():
        txt(f"vs{key}N", f"{c} | {tot_sv}")
        n.demo_bar(by, f"vs{key}", RX0, RX1, c / float(tot_sv))
    txt("voteNeeded", "25")


def demo(page, stage, maps=None, votes=None, focus=0, mine=-1, roster=None, booster=0, secs=None, tip=0, day=6, me=-1, decided=False):
    """A copy of the page set up the way t2r-ext fills it, for widget_preview.
    stage: 0 waiting, 1 vote, 2 booster, 3 loading."""
    w = copy.deepcopy(page)
    by = {i["name"]: i for i in w["items"]}
    nr.demo_chips(by)

    def vis(n, on):
        by[n]["visible"] = int(bool(on))

    def txt(n, s):
        by[n]["text"] = s
    if stage == 3:                                  # page = the loading widget (add_loading_screen)
        vis("t2r", 1); vis("fader", 0)
    else:
        vis("vote", stage <= 1); vis("booster", stage == 2)
        vis("mapBg", stage >= 2); vis("mapShade", stage >= 2)
        vis("bgDark", stage >= 2); vis("bgLight", stage >= 2)
        for nm in ("bgDim", "hdPar", "hdDiv", "hdPage", "hdRule", "wkDay", "wkSv", "wkBarT", "wkBarF", "pbar") + ("bgVig",) + tuple(f"wkTk{k}" for k in range(1, 12)):
            vis(nm, stage <= 1)
    maps = maps or [("School", "school"), ("Gas Station", "gas-station"), ("Shipyard", "shipyard")]
    votes = votes or [2, 1, 0]
    roster = roster or [("THEPIGGEHNINJA", 1, 1, 0), ("cdr1988", 1, 1, 0), ("KB-1993", 0, 1), ("southprak", 1, 1, 1),
                        ("Marlene", 0, 2), ("ash_fall", 1, 2, 2), ("TOBIAS", 0, 2), ("v0id", 0, 2)]
    t = secs
    if stage <= 1:
        if stage == 0:
            vis("voteWait", 1); vis("voteHead", 0); vis("voteTiles", 0); vis("voteGrid", 0); vis("voteRoster", 0)
            vis("voteTimer", 0)
            import nr_style as n
            n.demo_header(by, "FOUND MATCH", "MAP VOTE"); n.demo_week(by, day // 7 + 1, day % 7 + 1, day)
            n.demo_prompts(by, [("SPACE", "VOTE", 0, "menu:select")])
        else:
            _demo_vote(by, maps, votes, focus, mine, roster, me, t if t is not None else 11, day, decided=decided)
    elif stage == 2:
        txt("boTitle", 'Choose One-Use Booster - <font color="#e04848">00:%02d</font>' % (t if t is not None else 4))
        for k in range(BOOSTERS):
            vis(f"boSel{k}", k == booster)
            by[f"boName{k}"]["tint.start"] = by[f"boName{k}"]["tint.end"] = TEXT if k == booster else 0xFF8E8A84
        txt("boBigName", BOOSTER_LIST[booster][0]); txt("boDesc", BOOSTER_LIST[booster][1])
        by["boBig"]["path"] = BOOSTER_ICON.format(BOOSTER_SLUGS[booster])
        for k in range(BOOSTERS):
            by[f"boIcon{k}"]["path"] = BOOSTER_ICON.format(BOOSTER_SLUGS[k])
    else:
        nm, img = maps[0]
        by["mapBg"]["path"] = MAPIMG.format(img); by["ldPic"]["path"] = TIP_PIC % (tip % len(TIPS) + 1, 1)
        txt("ldMap", nm); txt("ldMode", "SUPPLY RAID"); txt("ldSpin", "LOADING...")
        cat, tx = TIPS[tip % len(TIPS)]
        txt("ldTipCat", cat + ":"); txt("ldTip", tx)
    return w


def preview(wdir, out_dir):
    """python matchflow_widgets.py <extracted widget dir> <out dir>"""
    import tempfile
    import sys as _t2s, os as _t2o; _t2s.path.append(_t2o.path.normpath(_t2o.path.join(_t2o.path.dirname(_t2o.path.abspath(__file__)), "..")))  # noqa: E402
    import t2rtmp  # noqa: F401  (temp hygiene: tools/t2rtmp.py)
    import widget_preview
    main = json.load(open(os.path.join(wdir, "to1", "main-menu.widget.json"), encoding="utf-8"))
    page = build_page(main)
    ltmp = tempfile.mkdtemp()
    os.makedirs(os.path.join(ltmp, "to1"))
    import shutil
    shutil.copy(os.path.join(wdir, LOADING + ".widget.json"), ltmp)
    shutil.copy(os.path.join(wdir, "to1", "main-menu.widget.json"), os.path.join(ltmp, "to1"))
    add_loading_screen(ltmp)
    loading = json.load(open(os.path.join(ltmp, LOADING + ".widget.json"), encoding="utf-8"))
    shots = (("matchflow-0-waiting", dict(stage=0)),
             ("matchflow-1-vote", dict(stage=1)),
             ("matchflow-2-booster", dict(stage=2, booster=3)),
             ("matchflow-3-loading", dict(stage=3, tip=0, maps=[("School", "school")] * 3)))
    os.makedirs(out_dir, exist_ok=True)
    outs = []
    for name, kw in shots:
        tmp = os.path.join(tempfile.gettempdir(), "t2r-" + name + ".widget.json")
        json.dump(demo(loading if kw["stage"] == 3 else page, **kw), open(tmp, "w", encoding="utf-8"))
        outs.append(widget_preview.render(tmp, os.path.join(out_dir, name + ".png")))
        print(outs[-1])
    compare(outs, os.path.join(out_dir, "matchflow-compare-2013.png"))


def preview_vote(wdir, out_dir):
    """vote previews (python matchflow_widgets.py <extracted widget dir> <out dir> vote): 3 maps, more than 3 maps, after our vote, waiting."""
    import tempfile
    import sys as _t2s, os as _t2o; _t2s.path.append(_t2o.path.normpath(_t2o.path.join(_t2o.path.dirname(_t2o.path.abspath(__file__)), "..")))  # noqa: E402
    import t2rtmp  # noqa: F401
    import widget_preview
    main = json.load(open(os.path.join(wdir, "to1", "main-menu.widget.json"), encoding="utf-8"))
    page = build_page(main)
    os.makedirs(out_dir, exist_ok=True)
    bg = nr.preview_bg(os.path.join(out_dir, "_bg.png"))
    allmaps = [("School", "school"), ("Gas Station", "gas-station"), ("Shipyard", "shipyard"), ("Forest", "forest"), ("Capitol Hill Shops", "forest"),
               ("Convention Center", "school"), ("Bookstore (Part I)", "shipyard"), ("Supermarket", "gas-station"), ("Hospital", "forest"),
               ("Train Station", "school"), ("Boardwalk", "shipyard"), ("Courtroom", "forest"), ("Overpass", "gas-station"), ("Plaza (Part I)", "school")]
    shots = (("vote-3maps", dict(stage=1, focus=1, votes=[2, 1, 0], me=1)),
             ("vote-voted", dict(stage=1, focus=0, mine=0, votes=[3, 1, 0], me=1, secs=7)),
             ("vote-grid", dict(stage=1, maps=allmaps, focus=4, mine=2, votes=[0, 1, 2, 0, 3, 0, 0, 0, 0, 1, 0, 0, 1, 0], me=3, secs=9)),
             ("vote-waiting", dict(stage=0)))
    for name, kw in shots:
        tmp = os.path.join(tempfile.gettempdir(), "t2r-" + name + ".widget.json")
        json.dump(demo(page, **kw), open(tmp, "w", encoding="utf-8"))
        print(widget_preview.render(tmp, os.path.join(out_dir, name + ".png"), scale=0.5, bg=bg))


REFS = ("matchfoundwaitingplayers", "matchfound2mapvote", "matchfound4boosterselectnoloadyet", "matchfound5loadingandtip")


def compare(outs, dst):
    """Sheet: the 2013 screenshots (2013matchflow/, here or in the main checkout) beside our previews."""
    from PIL import Image
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    for d in (os.path.join(root, "2013matchflow"), os.path.join(root, "..", "..", "..", "2013matchflow")):
        if os.path.isdir(d):
            break
    else:
        return
    W, H = 640, 360
    sheet = Image.new("RGB", (W * 2 + 30, (H + 10) * len(REFS) + 10), (40, 40, 44))
    for r, (ref, ours) in enumerate(zip(REFS, outs)):
        f = os.path.join(d, ref + ".png")
        if not os.path.exists(f):
            continue
        sheet.paste(Image.open(f).convert("RGB").resize((W, H), Image.LANCZOS), (10, 10 + r * (H + 10)))
        sheet.paste(Image.open(ours).convert("RGB").resize((W, H), Image.LANCZOS), (W + 20, 10 + r * (H + 10)))
    sheet.save(dst)
    print(dst)


if __name__ == "__main__":
    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    if len(sys.argv) > 3 and sys.argv[3] == "vote":
        preview_vote(sys.argv[1], sys.argv[2])
    else:
        preview(sys.argv[1], sys.argv[2])
