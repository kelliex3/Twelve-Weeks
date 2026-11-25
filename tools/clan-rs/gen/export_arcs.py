#!/usr/bin/env python3
"""Story-arc text tables (tools/clan_arcs_text*.py) for the Polish / English rendering of stored "arc/<kind>.<node>" feed rows -> data/arcs.json.
usage: export_arcs.py <tools dir> [out.json]"""
import json
import os
import sys

src = os.path.abspath(sys.argv[1])
sys.path.insert(0, src)
import clan_arcs_text as AT

out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "arcs.json")
d = {"text": {k: list(v) for k, v in AT.TEXT.items()},
     "rel": {k: {g: list(e) for g, e in v.items()} for k, v in AT.REL.items()},
     "tasks": [list(x) for x in AT.TASKS], "places": [list(x) for x in AT.PLACES], "wishes": [list(x) for x in AT.WISHES],
     "spots": [list(x) for x in AT.SPOTS], "keeps": {k: list(v) for k, v in AT.KEEPS.items()}}
with open(out, "w", encoding="utf-8", newline="\n") as f:
    json.dump(d, f, ensure_ascii=False, separators=(",", ":"))
print("wrote %s (%d bytes)" % (out, os.path.getsize(out)))
