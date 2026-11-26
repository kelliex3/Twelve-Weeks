"""k6_diff2.py PY RS IN [N] : the first N differing lines that are not http ops, as differing paths"""
import json, os, sys
IGN = [x for x in os.environ.get("K6_IGNORE", "").split(",") if x]
a = [json.loads(x) for x in open(sys.argv[1])]
b = [json.loads(x) for x in open(sys.argv[2])]
ins = [json.loads(x) for x in open(sys.argv[3])]
lim = int(sys.argv[4]) if len(sys.argv) > 4 else 3
def walk(p, u, v, out):
    if type(u) != type(v): out.append((p, u, v)); return
    if isinstance(u, dict):
        for k in sorted(set(u) | set(v)):
            if k not in u or k not in v: out.append((p + "/" + k, u.get(k, "<none>"), v.get(k, "<none>")))
            else: walk(p + "/" + k, u[k], v[k], out)
    elif isinstance(u, list):
        if len(u) != len(v): out.append((p, u, v)); return
        for j, (q, w) in enumerate(zip(u, v)): walk(p + "[%d]" % j, q, w, out)
    elif u != v: out.append((p, u, v))
n = 0
for i, (x, y) in enumerate(zip(a, b)):
    if x == y: continue
    op = ins[i]["op"]
    if op == "http" and "-http" not in sys.argv: continue
    n += 1
    if n > lim: break
    out = []
    walk("", x, y, out)
    out = [o for o in out if not any(g in o[0] for g in IGN)]
    if not out:
        n -= 1
        continue
    print("line", i + 1, op, json.dumps(ins[i])[:160])
    for p, u, v in out[:5]: print("  ", p, "\n    py:", json.dumps(u)[:300], "\n    rs:", json.dumps(v)[:300])
print("lines", len(a), len(b))
