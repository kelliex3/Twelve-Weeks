"""k3_diff.py PY RS : first differing line of two harness outputs, as the differing paths"""
import json, sys
a = [json.loads(x) for x in open(sys.argv[1])]
b = [json.loads(x) for x in open(sys.argv[2])]
def walk(x, y, p, out):
    if type(x) != type(y):
        out.append((p, x, y)); return
    if isinstance(x, dict):
        for k in sorted(set(x) | set(y)):
            if k not in x or k not in y: out.append((p + "/" + k, x.get(k, "<none>"), y.get(k, "<none>")))
            else: walk(x[k], y[k], p + "/" + k, out)
    elif isinstance(x, list):
        if len(x) != len(y): out.append((p, x, y)); return
        for i, (u, v) in enumerate(zip(x, y)): walk(u, v, p + "[%d]" % i, out)
    elif x != y:
        out.append((p, x, y))
shown = 0
for i, (x, y) in enumerate(zip(a, b)):
    out = []
    walk(x, y, "", out)
    if out:
        print("line", i + 1)
        for p, u, v in out[:6]:
            print(" ", p, "\n    py:", json.dumps(u)[:500], "\n    rs:", json.dumps(v)[:500])
        shown += 1
        if shown >= int(sys.argv[3]) if len(sys.argv) > 3 else 1: break
