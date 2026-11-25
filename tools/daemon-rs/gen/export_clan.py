#!/usr/bin/env python3
"""Export every module-level data value of the deployed clan engine (clan*.py, achievements.py) to data/clan_tables.json,
so the Rust clan engine uses the exact same text and numbers (no hand copies).

usage: python3 gen/export_clan.py <dir with the deployed .py files> data/clan_tables.json

Format (JSON; modules sorted by name, every dict keeps its Python insertion order): {"<module>": {"<NAME>": <value>}} with
  str / int / float (repr round-trip) / bool / null / list (list AND tuple) as plain JSON,
  {"$d": [[key, value], ...]}   dict whose keys are not all str (insertion order kept),
  {"$s": [...]}                 set / frozenset (sorted by repr, Python never relies on set order),
  {"$re": "<pattern>", "f": n}  compiled regex (pattern + re flags),
  {"$b": "<hex>"}               bytes,
  {"$fn": "mod.qualname"} / {"$mod": "name"}  a function / module inside a table (the Rust port codes it by hand).
Strings starting with the export directory have it replaced by "@HERE@".
A plain dict with str keys stays a JSON object (insertion order kept).
Callables, classes, modules and private caches the code fills at runtime are skipped; see SKIP.
"""
import json, os, re, sys, types

src = os.path.abspath(sys.argv[1])
out = os.path.abspath(sys.argv[2])
sys.path.insert(0, src)
os.chdir(src)

MODS = sorted(f[:-3] for f in os.listdir(src) if f.endswith(".py") and (f.startswith("clan") or f == "achievements.py"))
SKIP_MODS = {"clan_ingame_dump", "clan_demo_seed", "clan_reset_cli"}


def enc(v, path):
    if isinstance(v, str):
        return v.replace(src, "@HERE@")       # file paths built from __file__: the Rust side joins them to its own host dir
    if v is None or isinstance(v, bool):
        return v
    if isinstance(v, int):
        return v
    if isinstance(v, float):
        return v
    if isinstance(v, (list, tuple)):
        return [enc(x, path) for x in v]
    if isinstance(v, dict):
        if all(isinstance(k, str) for k in v):
            return {k: enc(x, path) for k, x in v.items()}
        return {"$d": [[enc(k, path), enc(x, path)] for k, x in v.items()]}
    if isinstance(v, (set, frozenset)):
        return {"$s": [enc(x, path) for x in sorted(v, key=repr)]}
    if isinstance(v, re.Pattern):
        return {"$re": v.pattern, "f": int(v.flags)}
    if isinstance(v, bytes):
        return {"$b": v.hex()}
    if isinstance(v, types.ModuleType):
        return {"$mod": v.__name__}
    if callable(v):
        return {"$fn": getattr(v, "__module__", "?") + "." + getattr(v, "__qualname__", "?")}
    raise TypeError("%s: cannot export %r" % (path, type(v)))


data = {}
skipped = []
for m in MODS:
    if m in SKIP_MODS:
        continue
    mod = __import__(m)
    d = {}
    for k, v in vars(mod).items():
        if k.startswith("__"):
            continue
        if isinstance(v, (types.ModuleType, types.FunctionType, types.BuiltinFunctionType, type, types.MethodType)) or callable(v):
            continue
        try:
            d[k] = enc(v, "%s.%s" % (m, k))
        except TypeError as e:
            skipped.append(str(e))
    data[m] = d

with open(out, "w", encoding="utf-8", newline="\n") as f:
    json.dump(data, f, ensure_ascii=False, separators=(",", ":"))      # no sort_keys: dict order is Python insertion order (rng.choices over list(d) depends on it)
    f.write("\n")
print("modules", len(data), "values", sum(len(x) for x in data.values()), "bytes", os.path.getsize(out))
for s in skipped:
    print("SKIP", s)
