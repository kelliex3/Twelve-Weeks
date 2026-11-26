"""k2_diff.py PY RS : the first differing line of two k2 outputs, as the paths that differ (up to 12)."""
import json
import sys


def walk(a, b, p, out):
    if len(out) >= 12:
        return
    if type(a) != type(b):
        out.append((p, a, b))
    elif isinstance(a, dict):
        for k in sorted(set(a) | set(b)):
            if k not in a or k not in b:
                out.append((p + "/" + k, a.get(k, "<missing>"), b.get(k, "<missing>")))
            else:
                walk(a[k], b[k], p + "/" + k, out)
    elif isinstance(a, list):
        if len(a) != len(b):
            out.append((p + "[len]", a if len(str(a)) < 400 else str(a)[:400], b if len(str(b)) < 400 else str(b)[:400]))
        for i, (x, y) in enumerate(zip(a, b)):
            walk(x, y, "%s[%d]" % (p, i), out)
    elif a != b:
        out.append((p, a, b))


pa, pb = open(sys.argv[1]).read().splitlines(), open(sys.argv[2]).read().splitlines()
for i in range(max(len(pa), len(pb))):
    if i >= len(pa) or i >= len(pb):
        print("length differs", len(pa), len(pb))
        break
    if pa[i] != pb[i]:
        out = []
        walk(json.loads(pa[i]), json.loads(pb[i]), "", out)
        print("line", i + 1)
        for p, x, y in out:
            print(" ", p, "\n    py:", str(x)[:600], "\n    rs:", str(y)[:600])
        break
else:
    print("identical")
