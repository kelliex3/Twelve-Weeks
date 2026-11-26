"""Differing lines of two clan_cl6 outputs, with the script line that made them:  clan_cl6_diff.py in py rs [how many details, default 3]"""
import json
import sys

a, b, c = (open(p, encoding="utf-8").read().split("\n") for p in sys.argv[1:4])
show = int(sys.argv[4]) if len(sys.argv) > 4 else 3
bad = [i for i, (x, y) in enumerate(zip(b, c)) if x != y]
print("differing lines:", len(bad), "of", len(b), "(rs has", len(c), "lines)", bad[:20])
for i in bad[:show]:
    x, y = b[i], c[i]
    print("line", i)
    if i < len(a):
        print("IN ", a[i][:500])
    try:
        jx, jy = json.loads(x), json.loads(y)
        if isinstance(jx, dict) and isinstance(jy, dict):
            for k in sorted(set(jx) | set(jy)):
                if jx.get(k) != jy.get(k):
                    sx, sy = json.dumps(jx.get(k)), json.dumps(jy.get(k))
                    n = 0
                    while n < min(len(sx), len(sy)) and sx[n] == sy[n]:
                        n += 1
                    print("  key", k, "PY", sx[max(0, n - 120):n + 200])
                    print("  key", k, "RS", sy[max(0, n - 120):n + 200])
            continue
    except ValueError:
        pass
    n = 0
    while n < min(len(x), len(y)) and x[n] == y[n]:
        n += 1
    print("PY ", x[max(0, n - 150):n + 250])
    print("RS ", y[max(0, n - 150):n + 250])
