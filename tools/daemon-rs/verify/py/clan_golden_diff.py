"""First difference of two clan_golden outputs: python3 clan_golden_diff.py script py.out rs.out (shows the event, the two replies, and for a mark the tables / rows that differ)."""
import json
import sys

sc = [json.loads(x) for x in open(sys.argv[1], encoding="utf-8") if x.strip()]
a = open(sys.argv[2], encoding="utf-8").read().split("\n")
b = open(sys.argv[3], encoding="utf-8").read().split("\n")
for n in range(max(len(a), len(b))):
    x = a[n] if n < len(a) else None
    y = b[n] if n < len(b) else None
    if x == y:
        continue
    print("line %d differs" % n)
    if n < len(sc):
        print("event:", json.dumps(sc[n])[:600])
    try:
        jx, jy = json.loads(x), json.loads(y)
    except Exception:
        print("py:", (x or "")[:500])
        print("rs:", (y or "")[:500])
        break
    if isinstance(jx, dict) and isinstance(jy, dict) and "rows" in jx and "rows" in jy:
        for t in sorted(set(jx["rows"]) | set(jy["rows"])):
            ra, rb = jx["rows"].get(t, []), jy["rows"].get(t, [])
            if ra != rb:
                sa, sb = set(ra), set(rb)
                print("table", t, "rows py", len(ra), "rs", len(rb))
                for r in sorted(sa - sb)[:3]:
                    print("  only py:", r[:400])
                for r in sorted(sb - sa)[:3]:
                    print("  only rs:", r[:400])
        break
    print("py:", json.dumps(jx)[:900])
    print("rs:", json.dumps(jy)[:900])
    break
