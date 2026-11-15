"""The brand (Twelve Weeks) must show in EVERY game language. The boat-menu button and the "THE LAST OF US ..." titles are literal widget texts
(cipherId empty = the same text in all 22 languages of the game); this test makes sure the rename keeps them literal and that loc_widgets / the language
tables never turn them into a translated label, and that no translation file carries the old name.
Run: python tools/tests/test_brand_langs.py   (synthetic widget pages, no game files)"""
import glob, json, os, re, shutil, sys, tempfile
OLD_NAME = re.compile(r"factions\s*(i{2}|2)\b", re.I)   # the old product name
HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)
sys.path.insert(0, os.path.join(TOOLS, "features"))
import t2rtmp  # noqa: E402,F401
FAILS = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: FAILS.append(what)

import matchflow_widgets as m  # noqa: E402
import loc_widgets  # noqa: E402

d = tempfile.mkdtemp(prefix="t2r_brand_")
try:
    def page(rel, items):
        p = os.path.join(d, *rel.split("/")) + ".widget.json"
        os.makedirs(os.path.dirname(p), exist_ok=True)
        json.dump({"items": items}, open(p, "w", encoding="utf-8"))
        return p
    T = lambda name, text: {"name": name, "text": text, "cipherId": "", "pointSize": 40}
    gs = page("menu/page-game-select", [T("buttonMultiplayer", "MULTIPLAYER"), T("earlyAccessNotice", "x")])
    fm = page("to1/find-match-menu", [T("gameTitle", "THE LAST OF US MP"), T("versionText", "v")])
    lb = page("to1/leaderboard", [T("gameTitle", "THE LAST OF US MP"), T("versionText", "v")])
    lo = page("to1/loadout-root", [T("gameTitle", "THE LAST OF US Multiplayer")])
    m.hide_notice_and_version(d)
    m.rename_brand(d)
    loc_widgets.apply(d, "pl")          # the pass that turns known labels into per-language ciphers
    seen = {}
    for f in (gs, fm, lb, lo):
        for i in json.load(open(f, encoding="utf-8"))["items"]:
            if i["name"] in ("buttonMultiplayer", "gameTitle"):
                seen[(os.path.basename(f), i["name"])] = i
    check(seen[("page-game-select.widget.json", "buttonMultiplayer")]["text"] == "TWELVE WEEKS", "boat button reads TWELVE WEEKS")
    for k, i in seen.items():
        check(not i.get("cipherId") and "TWELVE WEEKS" in i["text"].upper(), "literal in every language (no cipher): %s %s -> %r" % (k[0], k[1], i["text"]))
        check("FACTIONS" not in i["text"].upper(), "no old name left: %s" % k[0])
finally:
    shutil.rmtree(d, ignore_errors=True)

# translation files and cipher registry never carry the old name
bad = []
for f in glob.glob(os.path.join(TOOLS, "loc", "*", "*.txt")):
    for ln in open(f, encoding="utf-8"):
        if OLD_NAME.search(ln):
            bad.append(os.path.basename(f))
check(not bad, "tools/loc/<lang>/*.txt: no old product name (%s)" % ",".join(sorted(set(bad))))
check(not any(OLD_NAME.search(v) for v in loc_widgets.REGISTRY.values()), "widget cipher registry has no old name")
print("%d failure(s)" % len(FAILS))
sys.exit(1 if FAILS else 0)
