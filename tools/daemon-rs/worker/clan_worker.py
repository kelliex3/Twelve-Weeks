#!/usr/bin/env python3
"""Clan worker of the Rust t2r-daemon (phase 1 of the port).

The Rust daemon owns matchmaking, sessions and every daemon table. The clan engine (clan.py, clan_story.py, clan_perks.py, clan_status.py,
clan_ingame.py, clan_reset.py, achievements.py and their text tables) stays Python for now, unchanged, so every clan row and every story line
stays byte-identical. This process imports those modules and runs the daemon's clan glue (the code of daemon.py that used to call them,
copied here verbatim, with the daemon state it read passed in as arguments).

Protocol: one JSON object per line on stdin {"id": n, "fn": "<name>", "a": {...}}, one per line on stdout
{"id": n, "ok": true, "r": <result>, "log": ["line", ...]} or {"id": n, "ok": false, "err": "<repr>", "log": [...]}.
`log` = the daemon log lines the call produced as [category, body] (no timestamp; the daemon writes them with its own clock and rate limit).
Each call that writes runs while the daemon holds its DB lock, inside one transaction (commit on success, rollback on error), like
`with _dblock, dbc() as c:` did. Reads use a read-only connection.

Started by t2r-daemon from the host directory (the clan modules are imported from there). Exits when stdin closes.
"""
import json, os, sqlite3, sys, time, traceback

sys.path.insert(0, os.getcwd())
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

DB_PATH = os.environ.get("T2R_DB") or os.path.join(os.getcwd(), "t2r.db")

_seed = os.environ.get("T2R_TEST_SEED", "").strip()
if _seed:                                    # differential tests: the same draws as the Python daemon's clan modules (verify/py/seedhook.py)
    import random as _random
    _R = _random.Random(int(_seed) + 1)
    for _n in ("random", "choice", "choices", "shuffle", "randint", "randrange", "sample", "uniform"):
        setattr(_random, _n, getattr(_R, _n))

_LOG = []
_CTRL = {i: " " for i in range(32)}
_CTRL.update({0x7f: " ", 0x2028: " ", 0x2029: " ", 0x85: " "})


def log(*a):
    """daemon.py log(): the line body (the daemon adds '[HH:MM:SS] ', the category rate limit and the queue)."""
    if not a:
        return
    cat = str(a[0])[:24] if a[0] != "EVT" or len(a) < 2 else "EVT " + str(a[1])[:20]
    _LOG.append([cat, " ".join(str(x) for x in a).translate(_CTRL)])


def evt(kind, *a):
    log("EVT", kind, *a)


_FN = {}


def fn(name=None, write=False):
    def deco(f):
        _FN[name or f.__name__] = (f, write)
        return f
    return deco


_rw = [None]
_ro = [None]


def _conn(write):
    """A long-lived connection per kind (the daemon's dbc() opened one per call; the rows are the same)."""
    if write:
        if _rw[0] is None:
            c = sqlite3.connect(DB_PATH, timeout=10)
            c.execute("PRAGMA synchronous=FULL")
            _rw[0] = c
        return _rw[0]
    if _ro[0] is None:
        path = os.path.abspath(DB_PATH).replace("\\", "/")
        _ro[0] = sqlite3.connect("file:%s?mode=ro" % path, uri=True, timeout=10)
    return _ro[0]


@fn()
def ping(c, a):
    return {"pong": True, "pid": os.getpid()}


# ---- the clan glue registers here (import of the per-area glue modules) ----
try:
    import clan_worker_glue            # noqa: F401  (tools/daemon-rs/worker/clan_worker_glue.py: @fn functions)
except ImportError as _e:
    sys.stderr.write("clan_worker: no glue module: %r\n" % (_e,))


def serve():
    out = sys.stdout
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except ValueError:
            continue
        rid = req.get("id")
        del _LOG[:]
        ent = _FN.get(req.get("fn"))
        if ent is None:
            res = {"id": rid, "ok": False, "err": "unknown fn %r" % (req.get("fn"),), "log": []}
        else:
            f, write = ent
            c = None
            try:
                c = _conn(write)
                r = f(c, req.get("a") or {})
                if write:
                    c.commit()
                res = {"id": rid, "ok": True, "r": r, "log": list(_LOG)}
            except BaseException as e:      # noqa: BLE001 - one bad call never ends the worker
                if write and c is not None:
                    try:
                        c.rollback()
                    except sqlite3.Error:
                        pass
                if isinstance(e, sqlite3.Error):
                    for k in (_rw, _ro):     # a broken connection is reopened on the next call
                        try:
                            k[0] and k[0].close()
                        except sqlite3.Error:
                            pass
                        k[0] = None
                sys.stderr.write(traceback.format_exc())
                res = {"id": rid, "ok": False, "err": repr(e), "log": list(_LOG)}
        out.write(json.dumps(res, separators=(",", ":")) + "\n")
        out.flush()


if __name__ == "__main__":
    serve()
