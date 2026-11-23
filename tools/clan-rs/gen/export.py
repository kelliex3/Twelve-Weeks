#!/usr/bin/env python3
import json
import os
import re
import sys

src = os.path.abspath(sys.argv[1])
sys.path.insert(0, src)
os.environ["T2R_CLAN_ALIFE"] = "1"

import clan_alife as AL
import clan_alife_lex as X
import clan_chronicle as CH
import clan_chronicle_maps as MAPN
import clan_kids as KD
import clan_story as CS
import clan_story_data as D
import clan_story_good_rich as RG
import clan_story_pl as PLN
import clan_story_solo as SO

LETTER_RE = re.compile(r"\{([ABCD])(?:_\w+)?\}")


def kid_letters(*tpls):
    out = set()
    for t in tpls:
        if t:
            out |= set(LETTER_RE.findall(t))
    return "".join(sorted(out))


def combat2(en, pl):
    return bool(KD.combat_text(en, "en") or (pl and KD.combat_text(pl, "pl")))


def tpl_info(en):
    pl = CS._pl_twin(en)
    return {"l": "".join(sorted(PLN.tpl_letters(en))), "k": kid_letters(en, pl), "c": combat2(en, pl), "w": bool(SO.CROWD_RE.search(en)),
            "s": any(k in en for k in D.TICKER_NOT_SERA)}


tpls = {}


def note(t):
    if t not in tpls:
        tpls[t] = tpl_info(t)
    return t


for _sc, en, _pl in RG.ROWS:
    note(en)
for t in list(D.TICKER_HEALTHY) + list(D.TICKER_SICK) + list(D.TICKER_HUNGRY) + [D.TICKER_RARE_SICK]:
    note(t)
for t, _pool in CS._KID_PLAIN.values():
    note(t)
for rows in SO.SOLO.values():
    for _sc, t in rows:
        note(t)

story = {
    "first": list(D.FIRST_2013), "female": sorted(D.FEMALE_2013), "extra_w": [list(x) for x in D.EXTRA_WLF], "extra_s": [list(x) for x in D.EXTRA_SERA],
    "neutral": sorted(D.NEUTRAL_2013), "last": list(D.LAST_2013),
    "roles": [[k, v[0], v[1], v[2]] for k, v in D.ROLES.items()],
    "illness": [[k] + list(v) for k, v in D.ILLNESS.items()],
    "faction": {fk: {k: D.FACTION[fk][k] for k in ("plural", "enemy", "enemies", "leader", "places")} for fk in ("W", "S")},
    "infected": list(D.INFECTED), "rare": D.TICKER_RARE_SICK, "rare_chance": D.TICKER_RARE_CHANCE,
    "healthy": list(D.TICKER_HEALTHY), "sick": list(D.TICKER_SICK), "hungry": list(D.TICKER_HUNGRY),
    "kid_plain": {k: list(v) for k, v in CS._KID_PLAIN.items()},
    "rich": [[sc, en] for sc, en, _pl in RG.ROWS],
    "solo": {k: [list(x) for x in v] for k, v in SO.SOLO.items()},
    "tpl_pool": dict(SO.TPL_POOL), "cause_group": dict(SO.CAUSE_GROUP),
    "solo_death": {k: list(v) for k, v in CS._SOLO_DEATH.items()},
    "feeling": sorted(CS.FEELING_KINDS),
    "crowd_re": SO.CROWD_RE.pattern, "group_re": SO.GROUP_RE.pattern,
    "tpls": tpls,
    "pron": {g: dict(v) for g, v in CS.PRON.items()},
    "nums": CS.NUMS, "numw": CS.NUMW, "gainw": CS.GAINW,
}

pl = {
    "reg": {k: list(v) for k, v in PLN.REG.items()},
    "places": {k: list(v) for k, v in PLN.PLACES.items()}, "enemy": {k: list(v) for k, v in PLN.ENEMY.items()},
    "inf": {k: list(v) for k, v in PLN.INF.items()}, "ill": {k: list(v) for k, v in PLN.ILL.items()}, "cure": {k: list(v) for k, v in PLN.CURE.items()},
    "ago": dict(PLN.AGO), "card_nom": PLN._CARD_NOM, "card_gen": PLN._CARD_GEN, "special": dict(PLN.PL_SPECIAL),
    "roles": {k: [list(v[0]), list(v[1])] for k, v in PLN.ROLES.items()},
}

lex_noun = lambda t: {k: list(v) for k, v in t.items()}
alife_reg = {}
for tid, (pool, sc, tone, en, plt, lt, ve, vp) in AL.REG.items():
    alife_reg[tid] = {"s": sc, "t": tone, "en": en, "pl": plt, "l": "".join(sorted(lt)), "ve": sorted(ve), "vp": sorted(vp), "k": kid_letters(en, plt),
                      "c": combat2(en, plt)}
alife = {
    "reg": alife_reg, "by_pool": {k: list(v) for k, v in AL.BY_POOL.items()},
    "traits": AL.TRAIT_IDS, "fact_w": AL.FACT_W, "tail_ok": list(AL.TAIL_OK), "persistent": list(AL.PERSISTENT), "daily_facts": AL.DAILY_FACTS,
    "ticker_share": AL.TICKER_SHARE, "min_clan": AL.MIN_CLAN, "mem_causes": list(AL.MEM_CAUSES),
    "places": {fk: lex_noun(v) for fk, v in X.PLACES.items()}, "goods": lex_noun(X.GOODS), "projects": lex_noun(X.PROJECTS), "groups": lex_noun(X.GROUPS),
    "injuries": lex_noun(X.INJURIES), "keeps": lex_noun(X.KEEPS), "jobs": [[k] + list(v) for k, v in X.JOBS.items()], "origins": [[k] + list(v) for k, v in X.ORIGINS.items()], "skills": lex_noun(X.SKILLS),
    "prog": {str(k): list(v) for k, v in X.PROG.items()}, "seasons": [list(s) for s in X.SEASONS], "weather": list(X.WEATHER),
    "job_combat": sorted(k for k, v in X.JOBS.items() if KD.combat_text(v[1], "en") or KD.combat_text(v[2], "pl")),
    "origin_combat": sorted(k for k, v in X.ORIGINS.items() if KD.combat_text(v[1], "en") or KD.combat_text(v[2], "pl")),
    "skill_combat": sorted(k for k, v in X.SKILLS.items() if KD.combat_text(v[1], "en")),
    "ze_re": X._ZE.pattern,
    "combat_en": KD._EN, "combat_pl": KD._PL, "combat_vars": sorted(KD._VARS),
}

chron = {
    "reg": {tid: [v[3], v[4]] for tid, v in CH.REG.items()},
    "whens": [list(w) for w in CH.WHENS], "use": {k: list(v) for k, v in CH.USE.items()}, "keep": {k: list(v) for k, v in CH.KEEP.items()},
    "map_short": dict(MAPN.EN_SHORT), "map_pl": dict(MAPN.PL),
}

out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "clan.json")
with open(out, "w", encoding="utf-8", newline="\n") as f:
    json.dump({"story": story, "pl": pl, "alife": alife, "chron": chron}, f, ensure_ascii=False, separators=(",", ":"), sort_keys=False)
print("wrote %s (%d bytes)" % (out, os.path.getsize(out)))
