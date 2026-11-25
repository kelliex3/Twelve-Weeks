#!/usr/bin/env python3
"""Tables the admin site's clan page needs on top of clan.json: the loot item / scene text (clan_loot*), the portrait head pool, the chronicle roster words, the
journal sentence pools (status cards), the status lines, the Polish roster words.  usage: export_admin.py <tools dir> [out.json]"""
import json
import os
import sys

src = os.path.abspath(sys.argv[1])
sys.path.insert(0, src)
import clan_chronicle as CH
import clan_journal as J
import clan_journal_text as JE
import clan_journal_text_pl as JP
import clan_loot as L
import clan_loot_data as D1
import clan_loot_data2 as D2
import clan_portrait_pool as PP
import clan_saga_pl_data as PD
import clan_status as ST
import clan_story_pl as PLN

d = {
    "loot": {"items": {k: [it[x] for x in L.KEYS] for k, it in L.ITEMS.items()}, "icons": sorted(L.LAUNCHER_ICONS), "fallback": D1.FALLBACK_ICON, "tiers": list(D1.TIERS),
             "conds": [list(c) for c in D2.CONDS], "micro": {k: [list(x) for x in v] for k, v in D2.MICRO.items()}, "hooks": [list(h) for h in D2.HOOKS],
             "names": list(D2.NAMES), "notes": [list(n) for n in D2.NOTES], "sent_en": list(D2.SENT_EN), "sent_pl": list(D2.SENT_PL),
             "places_pl": {k: v[1] for k, v in PD.PLACES.items()}},
    "portraits": {"pool": [list(r) for r in PP.POOL], "rendered": sorted(PP.RENDERED), "expr": {str(k): list(v) for k, v in PP.EXPR.items()}},
    "chron": {"tempers": CH.TEMPERS, "temper_label": {k: list(v) for k, v in CH.TEMPER_LABEL.items()}, "bond_types": CH.BOND_TYPES, "habit_short": [list(h) for h in CH.HABIT_SHORT],
              "habit_meta": [list(h) for h in CH.HABIT_META], "pasts": [list(p) for p in CH.PASTS], "bond_label": CH.BOND_LABEL, "bond_label_f": CH.BOND_LABEL_F, "icon": CH.ICON},
    "journal": {"en": {k: [list(x) for x in v] for k, v in JE.POOLS.items()}, "pl": {k: list(v) for k, v in JP.POOLS.items()}, "lack_en": JE.LACK, "lack_pl": JP.LACK,
                "no_article": list(JE.NO_ARTICLE), "num": list(J._NUM)},
    "status": {"names": ST.NAMES, "expr": {str(k): v for k, v in ST.EXPR.items()},
               "lines_en": {str(k): v for k, v in ST.LINES["en"].items()}, "lines_pl": {str(k): v for k, v in ST.LINES["pl"].items()}},
    "plabels": {"state": {k: list(v) for k, v in PLN.STATE.items()}, "mood": {k: list(v) for k, v in PLN.MOOD.items()}, "morale": PLN.MORALE, "ill": {k: list(v) for k, v in PLN.ILL.items()}},
}
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "admin.json")
with open(out, "w", encoding="utf-8", newline="\n") as f:
    json.dump(d, f, ensure_ascii=False, separators=(",", ":"))
print("wrote %s (%d bytes)" % (out, os.path.getsize(out)))
