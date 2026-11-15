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
TIPS = [
    ("WEAPONS", "The Molotov is great for flushing the enemy out of cover."),
    ("WEAPONS", "Shotguns are deadly up close but fall off fast. Keep moving and close the distance."),
    ("WEAPONS", "A suppressor keeps your shots quiet. Pick your fights."),
    ("WEAPONS", "The bow is silent. Pick your arrows back up after the fight."),
    ("SURVIVAL", "Crafting takes time. Make medkits and bombs before the fight, not during it."),
    ("SURVIVAL", "Supply boxes refill over time. Remember where they are."),
    ("SURVIVAL", "Armor soaks up damage from every direction. Buy it early."),
    ("TEAMWORK", "A downed teammate can be revived. Stay close and watch their back."),
    ("TEAMWORK", "Mark an enemy to show your whole team where they are."),
    ("TEAMWORK", "Stick together. A lone survivor is easy prey for the other faction."),
    ("STEALTH", "Stay low in the grass. A prone survivor is hard to spot."),
    ("STEALTH", "Listen mode shows you footsteps. Moving slowly keeps you off it."),
    ("COMBAT", "Finishing a downed enemy earns more parts. Watch for his friends."),
    ("COMBAT", "Headshots down enemies faster. Take your time and aim."),
    ("CLAN", "Every survivor in your clan eats. Gather supplies in matches to keep them fed."),
    ("CLAN", "Parts earned in a match are turned into supplies for your clan."),
]
# Daily mission placeholder (2013: a weapon challenge with three tiers and the survivors they bring)
MISSIONS = [("Shotgun Downs", "Downs with Shotgun"), ("Revolver Downs", "Downs with Revolver"),
            ("Bow Downs", "Downs with Bow"), ("Headshots", "Headshots"), ("Executions", "Executions"),
            ("Revives", "Revives")]
TIERS = [(3, 5), (5, 7), (7, 10)]


def _roster(b, parent, pre, place):
    """ROSTER rows <pre>0.. under `parent`; place(r) -> (x, y, width). The vote's roster ('ros', width 2262) and the all-maps grid's copy ('gros')."""
    for r in range(ROSTER):
        x, y, w = place(r)
        g = b.group(f"{pre}{r}", parent, b.tl(x, y, w, 80), visible=0)["guid"]
        b.rect(f"{pre}{r}Bar", g, b.fill(), 0xFFFFFFFF, 22)
        # team band: a soft gradient behind the name, full strength at the left edge easing out to the right (make_icons.team_grad,
        # same as the lobby rows); blue = our team, red = the enemy (t2r-ext mf_vote_fill); created before the name = drawn behind it
        b.image(f"{pre}{r}TabB", g, b.tl(0, 0, min(1100, w), 80), "streaming/t2r/icons/team-grad.png", 0xFF48A8D8, 200, visible=0)
        b.image(f"{pre}{r}TabR", g, b.tl(0, 0, min(1100, w), 80), "streaming/t2r/icons/team-grad.png", 0xFFE04848, 200, visible=0)
        rbw.add_to_builder(b, g, f"{pre}{r}Rk", f"{pre}{r}Rn", 52, 40, 48, 80)                                   # the rank badge (rank_badge_widgets; t2r-ext mf_vote_fill)
        b.text(f"{pre}{r}Name", g, b.tl(52 + rbw.reserved(48, 80), 0, w - 784 - rbw.reserved(48, 80), 80), "PLAYER", 48, 0xFFBDBDBD, tracking=2)
        b.add("Polyline", f"{pre}{r}Ck", g, **b.tl(w - 92, 18, 44, 40), **b.col(0xFFBDBDBD), closed=0, lineWidth=6.0,
              pointsUv=[0.05, 0.55, 0.38, 0.95, 0.97, 0.05], visible=0, **{"color.a": 255})


def build_page(main):
    """to1/matchflow.widget.json content."""
    t, root0, anim0 = _templates(main)
    b = Builder(t)
    root = copy.deepcopy(root0); root.update(guid=t2guid.guid())
    anim = copy.deepcopy(anim0); anim.update(guid=t2guid.guid(), parent=root["guid"])
    b.items += [root, anim]
    a = anim["guid"]
    b.image("bgBlur", a, b.bleed(), "general-textures/blur-background.png", 0xFFFFFFFF, 255)
    b.rect("bgDark", a, b.bleed(), 0xFF0B0C0D, 236)
    b.image("mapBg", a, b.fill(), MAPIMG.format("school"), 0xFFFFFFFF, 110, visible=0)
    b.rect("mapShade", a, b.bleed(), 0xFF000000, 150, visible=0)
    b.image("bgLight", a, b.ctr(1920, 980, 3600, 2200), "hud/player-menu/gradient/radial-gradient.png", 0xFFFFFFFF, 20)

    # ------------------------------------------------------------ stage 1: vote (matchfoundwaitingplayers, matchfound2mapvote)
    v = b.group("vote", a, b.tl(0, 0, 3840, 2160))["guid"]
    b.text("voteTitle", v, b.tl(300, 140, 1800, 150), "FOUND MATCH", 90, 0xFFD8D4CC, tracking=6)
    b.text("voteSub", v, b.tl(300, 285, 1300, 90), "SUPPLY RAID", 52, TEXT, tracking=4)
    b.text("voteTimer", v, b.tl(1330, 285, 1202, 90), "VOTE FOR A MAP - 00:15", 46, PM_DIM, align=2, tracking=3)
    # double buffer (timer flicker): the ext writes the hidden copy, then swaps
    b.text("voteTimerB", v, b.tl(1330, 285, 1202, 90), "VOTE FOR A MAP - 00:15", 46, PM_DIM, align=2, tracking=3, visible=0)
    b.text("voteWait", v, b.tl(600, 500, 1800, 100), "Found game, waiting for players...", 52, TEXT, tracking=2)
    TW, TH, TX, TY, GAP = 730, 411, 270, 400, 36
    tiles = b.group("voteTiles", v, b.tl(0, 0, 3840, 2160), visible=0)["guid"]
    for k in range(VOTE_MAX):
        x = TX + k * (TW + GAP)
        g = b.group(f"tile{k}", tiles, b.tl(x, TY, TW, TH))["guid"]
        b.rect(f"tile{k}Back", g, b.fill(), 0xFF050606, 255)
        b.image(f"tile{k}Img", g, b.fill(), MAPIMG.format("forest"), 0xFFFFFFFF, 255)
        b.rect(f"tile{k}Shade", g, b.fill(), 0xFF000000, 150)
        b.frame(f"tile{k}Frame", g, b.fill(), 0xFFFFFFFF, 230, 4.0)
        # 50 / tracking 2 ("Convention Center" stuck out of the tile at 62 / 3; longest names: Bookstore (Part I), Capitol Hill Shops)
        b.text(f"tile{k}Name", g, b.tl(24, 14, TW - 48, 100), "Forest", 50, TEXT, tracking=2)
        for j in range(CHECKS):
            c = b.add("Polyline", f"tile{k}Ck{j}", g, **b.tl(24 + j * 56, TH - 80, 44, 40), **b.col(0xFFFFFFFF),
                      closed=0, lineWidth=6.0, pointsUv=[0.05, 0.55, 0.38, 0.95, 0.97, 0.05], visible=0,
                      **{"color.a": 255})
    # the all-maps grid: GRID_COLS columns over the tiles' width (2262), 16:9 cells, + a 2-column roster under it
    GW = (TW * 3 + GAP * 2 - (GRID_COLS - 1) * 20) // GRID_COLS
    GH = GW * 9 // 16
    grid = b.group("voteGrid", v, b.tl(0, 0, 3840, 2160), visible=0)["guid"]
    for k in range(GRID_MAX):
        x, y = TX + (k % GRID_COLS) * (GW + 20), TY + (k // GRID_COLS) * (GH + 20)
        g = b.group(f"g{k}", grid, b.tl(x, y, GW, GH), visible=0)["guid"]
        b.rect(f"g{k}Back", g, b.fill(), 0xFF050606, 255)
        b.image(f"g{k}Img", g, b.fill(), MAPIMG.format("forest"), 0xFFFFFFFF, 255)
        b.rect(f"g{k}Shade", g, b.fill(), 0xFF000000, 150)
        b.frame(f"g{k}Frame", g, b.fill(), 0xFFFFFFFF, 230, 4.0, visible=0)
        b.text(f"g{k}Name", g, b.tl(16, 8, GW - 32, 70), "Forest", 36, TEXT, tracking=1)
        for j in range(CHECKS):
            b.add("Polyline", f"g{k}Ck{j}", g, **b.tl(16 + j * 44, GH - 52, 32, 30), **b.col(0xFFFFFFFF),
                  closed=0, lineWidth=5.0, pointsUv=[0.05, 0.55, 0.38, 0.95, 0.97, 0.05], visible=0, **{"color.a": 255})
    grows = TY + 4 * (GH + 20) + 10
    _roster(b, b.group("voteGridRoster", grid, b.tl(0, 0, 3840, 2160))["guid"], "gros",
            lambda r: (TX + (r % 2) * ((TW * 3 + GAP * 2) // 2 + 10), grows + (r // 2) * 86, (TW * 3 + GAP * 2) // 2 - 10))
    rows = b.group("voteRoster", v, b.tl(0, 0, 3840, 2160))["guid"]
    _roster(b, rows, "ros", lambda r: (TX, 905 + r * 86, 2262))
    # clan panel (the 2013 find match right column)
    b.line("voteDivider", v, 2619, 345, 2619, 1700, 0xFFFFFFFF, 160, 2.0)
    b.text("voteWeek", v, b.tl(2690, 420, 1000, 110), "WEEK 1: DAY 1", 62, PM_TEXT, tracking=4)
    b.line("voteWeekLine", v, 2690, 560, 3490, 560, 0xFFFFFFFF, 120, 2.0)
    b.image("voteSurvIcon", v, b.tl(2700, 620, 40, 86), PERSON_ICON, HEALTHY)
    b.text("voteSurv", v, b.tl(2776, 622, 700, 80), "5 SURVIVORS", 46, PM_TEXT, tracking=3)
    for k, (nm, txt, col) in enumerate((("voteHealthy", "HEALTHY: 5", HEALTHY), ("voteHungry", "HUNGRY: 0", HUNGRY),
                                        ("voteSick", "SICK: 0", SICK))):
        b.text(nm, v, b.tl(2690, 760 + k * 80, 700, 70), txt, 44, col, tracking=3)
    b.image("voteSupplyIcon", v, b.tl(2698, 1000, 56, 56), SUPPLY_ICON, 0xFFFFFFFF)
    b.text("voteNeeded", v, b.tl(2776, 990, 700, 76), 'NEEDED PER DAY: <font color="#3fa9f5">9</font>', 42, PM_TEXT, tracking=3)
    b.line("voteNeedLine", v, 2690, 1110, 3490, 1110, 0xFFFFFFFF, 120, 2.0)
    b.image("voteSelIcon", v, b.tl(270, 1898, 56, 56), "menu:select", 0xFFFFFFFF)
    b.text("voteSelText", v, b.tl(350, 1890, 500, 70), "VOTE", 46, PM_TEXT, tracking=4)
    b.text("voteHint", v, b.tl(900, 1890, 900, 70), "A / D  TO SWITCH", 38, PM_DIM, tracking=4)
    ph = b.group("voteHintPad", v, b.tl(900, 1890, 900, 70), visible=0)["guid"]       # pad version (t2r-ext shows it instead of voteHint while the last input was a pad)
    b.image("voteHintPrev", ph, b.tl(0, 7, 56, 56), "menu:prev", 0xFFFFFFFF)
    b.image("voteHintNext", ph, b.tl(70, 7, 56, 56), "menu:next", 0xFFFFFFFF)
    b.text("voteHintPadTx", ph, b.tl(150, 0, 750, 70), "TO SWITCH", 38, PM_DIM, tracking=4)

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

    if ui_r2_widgets.on("vote"):        # UI round 2 (B2): list + big map, huge countdown (re/ui-r2-matchflow-impl-1019.md)
        ui_r2_widgets.vote(b)
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
    b.image("ldPic", ld, b.tl(1620, 612, 1536, 858), MAPIMG.format("forest"), 0xFFFFFFFF, 255)
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


def rename_brand(wdir):
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
def demo(page, stage, maps=None, votes=None, focus=0, mine=-1, roster=None, booster=0, secs=None, tip=0, day=6):
    """A copy of the page set up the way t2r-ext fills it, for widget_preview.
    stage: 0 waiting, 1 vote, 2 booster, 3 loading."""
    w = copy.deepcopy(page)
    by = {i["name"]: i for i in w["items"]}

    def vis(n, on):
        by[n]["visible"] = int(bool(on))

    def txt(n, s):
        by[n]["text"] = s
    if stage == 3:                                  # page = the loading widget (add_loading_screen)
        vis("t2r", 1); vis("fader", 0)
    else:
        vis("vote", stage <= 1); vis("booster", stage == 2)
        vis("mapBg", stage >= 2); vis("mapShade", stage >= 2)
    maps = maps or [("School", "school"), ("Gas Station", "gas-station"), ("Shipyard", "shipyard")]
    votes = votes or [2, 1, 0]
    roster = roster or [("THEPIGGEHNINJA", 1), ("cdr1988", 1), ("KB-1993", 0), ("southprak", 1)]
    t = secs
    if stage <= 1:
        vis("voteWait", stage == 0); vis("voteTiles", stage == 1); vis("voteTimer", stage == 1)
        txt("voteTimer", 'VOTE FOR A MAP - <font color="#e04848">00:%02d</font>' % (t if t is not None else 11))
        for k in range(VOTE_MAX):
            nm, img = maps[k]
            on = k == focus
            txt(f"tile{k}Name", nm)
            by[f"tile{k}Img"]["path"] = MAPIMG.format(img)
            vis(f"tile{k}Frame", on); vis(f"tile{k}Shade", not on)
            for j in range(CHECKS):
                vis(f"tile{k}Ck{j}", j < votes[k])
        for r in range(ROSTER):
            vis(f"ros{r}", stage == 1 and r < len(roster))
            if r < len(roster):
                txt(f"ros{r}Name", roster[r][0])
                vis(f"ros{r}Ck", roster[r][1])
        txt("voteWeek", f"WEEK 1: DAY {day}")
        txt("voteSurv", "25 SURVIVORS"); txt("voteHealthy", "HEALTHY: 25"); txt("voteHungry", "HUNGRY: 0")
        txt("voteSick", "SICK: 0"); txt("voteNeeded", 'NEEDED PER DAY: <font color="#3fa9f5">25</font>')
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
        by["mapBg"]["path"] = MAPIMG.format(img); by["ldPic"]["path"] = MAPIMG.format(img)
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
    preview(sys.argv[1], sys.argv[2])
