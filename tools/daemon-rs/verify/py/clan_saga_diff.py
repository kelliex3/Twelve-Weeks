"""First differing line of a clan_saga differential run: clan_saga_diff.py in.N py.N rs.N [how many]"""
import json
import sys


def walk(a, b, path, out):
    if type(a) != type(b):
        out.append((path, a, b))
    elif isinstance(a, dict):
        ka, kb = list(a), list(b)
        if ka != kb:
            out.append((path + "/<keys>", ka, kb))
        for k in a:
            if k in b:
                walk(a[k], b[k], path + "/" + str(k), out)
    elif isinstance(a, list):
        if len(a) != len(b):
            out.append((path + "/<len>", len(a), len(b)))
        for i, (x, y) in enumerate(zip(a, b)):
            walk(x, y, path + "[%d]" % i, out)
    elif a != b:
        out.append((path, a, b))


ins = open(sys.argv[1], encoding="utf-8").read().splitlines()
py = open(sys.argv[2], encoding="utf-8").read().splitlines()
rs = open(sys.argv[3], encoding="utf-8").read().splitlines()
shown = 0
for i, (a, b) in enumerate(zip(py, rs)):
    if a != b:
        print("line", i, "op:", ins[i][:300] if i < len(ins) else "(final)")
        out = []
        walk(json.loads(a), json.loads(b), "", out)
        for p, x, y in out[:6]:
            print("  ", p, "\n     py:", json.dumps(x, ensure_ascii=False)[:300], "\n     rs:", json.dumps(y, ensure_ascii=False)[:300])
        shown += 1
        if shown >= int(sys.argv[4] if len(sys.argv) > 4 else 1):
            break
if len(py) != len(rs):
    print("lengths", len(py), len(rs))
