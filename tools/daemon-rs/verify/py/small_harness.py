#!/usr/bin/env python3
"""Python side of the differential test of the small modules: small_harness.py <names|notices|roles|rankbadge> < script

  T2R_PY_DIR      folder with the deployed names.py / notices.py / roles.py / rankbadge.py (default ~/rsverify/daemon/py)
  T2R_VERIFY_DIR  scratch folder (default: system temp); the run uses <it>/<module>_py

Reads the JSON-lines script (only the lines whose "m" is the module) and prints one JSON line per script line (the Rust side is
`t2r-daemon selftest <module>`; the same script gives the same bytes), then the database rows (names).
"""
import json
import os
import shutil
import sqlite3
import sys
import tempfile
import types

sys.stdout.reconfigure(encoding="utf-8", newline=chr(10))
sys.stdin.reconfigure(encoding="utf-8")
PYDIR = os.environ.get("T2R_PY_DIR") or os.path.expanduser("~/rsverify/daemon/py")
sys.path.insert(0, PYDIR)
MOD = sys.argv[1]
T = [0.0]
DIR = os.path.join(os.environ.get("T2R_VERIFY_DIR") or tempfile.gettempdir(), MOD + "_py")
shutil.rmtree(DIR, ignore_errors=True)
os.makedirs(DIR, exist_ok=True)


def dumps(x):
    return json.dumps(x, separators=(",", ":"))


def lines():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        v = json.loads(line)
        if v.get("m") != MOD:
            continue
        T[0] = float(v.get("t", 0.0))
        yield v


def st(v, k):
    return v[k] if isinstance(v.get(k), str) else ""


def do_names():
    import names
    names.time = types.SimpleNamespace(time=lambda: T[0])
    c = sqlite3.connect(os.path.join(DIR, "t.db"))
    for v in lines():
        op = v["op"]
        if op == "token_id":
            r = names.token_id(st(v, "token"))
        elif op == "clean":
            r = list(names.clean(st(v, "name"), st(v, "tag")))
        elif op == "load":
            r = list(names.load(c, st(v, "tid")))
        elif op == "save":
            lst = v.get("taken")
            r = list(names.save(c, st(v, "tid"), st(v, "name"), st(v, "tag"), taken=(lambda n: n in lst) if lst is not None else None))
            c.commit()
        elif op == "display":
            r = names.display(st(v, "name"), st(v, "tag"))
        elif op == "state_text":
            r = names.state_text(st(v, "name"), st(v, "tag"), v.get("err"))
        else:
            r = "?"
        print(dumps(r))
    for row in c.execute("SELECT token_id,name,tag,updated FROM names ORDER BY token_id").fetchall():
        print("ROW " + dumps(list(row)))


def do_notices():
    import notices
    n = notices.Notices(now=lambda: T[0])

    def items_json(items):
        return [list(x) for x in items]

    for v in lines():
        op = v["op"]
        if op == "clean":
            r = notices.clean(st(v, "s"), v.get("n", notices.MAX_TEXT))
        elif op == "add_text":
            r = n.add_text(st(v, "text"), v.get("minutes", 30.0), v.get("seconds"), v.get("text_pl"))
        elif op == "clear":
            r = n.clear(v.get("id", -1))
        elif op == "schedule":
            r = n.schedule(v.get("minutes", 0.0), v.get("reason", ""), v.get("restart", True), v.get("reason_pl"))
        elif op == "cancel":
            r = n.cancel()
        elif op == "go_final":
            n.go_final(v.get("hold", 60.0))
            r = None
        elif op == "due":
            r = n.due()
        elif op == "active":
            r = items_json(n.active(bool(v.get("stale")), bool(v.get("refused")), v.get("lang", "en")))
        elif op == "public":
            r = n.public()
        elif op == "reply_text":
            r = notices.Notices.reply_text([tuple(x) for x in v["items"]]).decode("utf-8", "replace")
        else:
            r = "?"
        print(dumps(r))


def do_roles():
    import roles
    d = DIR
    r_ = roles.Roles(d)
    for v in lines():
        op = v["op"]
        if op == "write":
            p = os.path.join(d, roles.FILES[v["kind"]])
            if v.get("hex") is not None:
                with open(p, "wb") as f:
                    f.write(bytes.fromhex(v["hex"]))
                if v.get("mt") is not None:
                    os.utime(p, ns=(v["mt"] * 10 ** 9, v["mt"] * 10 ** 9))
            else:
                try:
                    os.remove(p)
                except OSError:
                    pass
            r = None
        elif op == "role_of":
            r = r_.role_of(v.get("sid", 0))
        elif op == "parse":
            r = {str(k): w for k, w in roles.parse_ids(st(v, "text"), bool(v.get("skip_temp"))).items()}
        else:
            r = "?"
        print(dumps(r))


def do_rankbadge():
    import rankbadge
    for v in lines():
        op = v["op"]
        rows = [tuple(x) for x in v.get("rows", [])]
        if op == "norm":
            r = rankbadge.norm(st(v, "name"))
        elif op == "key":
            r = rankbadge.key(st(v, "name"))
        elif op == "wire":
            r = rankbadge.wire(rows)
        elif op == "ttl":
            rankbadge.TABLE_TTL = float(v.get("ttl", 3.0))
            r = None
        elif op == "table":
            r = rankbadge.table_text(lambda: rows, v.get("now", 0.0))
        else:
            r = "?"
        print(dumps(r))


{"names": do_names, "notices": do_notices, "roles": do_roles, "rankbadge": do_rankbadge}[MOD]()
