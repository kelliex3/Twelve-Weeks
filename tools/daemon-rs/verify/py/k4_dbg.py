import json
import os
import sys

sys.argv = ["x", "run"]
import k4_harness as h  # noqa: E402
from cseed import T, emit  # noqa: E402,F401

d = h.setup()
ops = [json.loads(line) for line in open(sys.argv[0] and os.environ["DBG_IN"])]
stop = int(os.environ["DBG_N"])
n = 0
for c in ops[:stop - 1]:
    T[0] = c.get("t", T[0])
    n += 1
    if c["op"] == "world":
        h.world_py(c["w"])
    else:
        h.mm_op(d, c)
T[0] = ops[stop - 1]["t"]
m = d.MM
print("queue", list(m.queue.items()))
print("lobbies", {k: (l.members, dict(l.away), l.hold, l.wait_until, l.src) for k, l in m.book.lobbies.items()})
print("of", m.book.of)
for lid, l in m.book.lobbies.items():
    for t in m.queue:
        if t not in m.book.of:
            print("fits", lid, t[:2], d.MM._book_fits(l.members, [t]), "cap", m._book_cap(l, [t]), "unit", m._book_unit(t))
print("search modes", {t[:2]: d._search_mode(t) for t in m.queue}, "forced", d.modeq.forced())
