"""Shared bits of the *_harness.py files of the lobby / queue / mode modules (lobbybook, requeue, modeq, gunlimit, interrogation, map_packs).

  python3 <module>_harness.py gen SEED N > script.jsonl     seeded call script (one JSON object per line)
  T2R_TEST_SEED=SEED python3 <module>_harness.py run < script.jsonl > out.py.jsonl
  T2R_TEST_SEED=SEED t2r-daemon selftest <module> < script.jsonl > out.rs.jsonl ; cmp out.py.jsonl out.rs.jsonl

The Python modules are imported from T2R_PY_DIR (the deployed .py files, default ~/rsverify/daemon/py). `secrets.token_hex` is
patched to the stream src/rnd.rs uses when T2R_TEST_SEED is set (one random.Random(seed), getrandbits(8) per byte). Time is frozen
per script line ("t").
"""
import json
import os
import random
import secrets
import sys

PYDIR = os.environ.get("T2R_PY_DIR") or os.path.expanduser("~/rsverify/daemon/py")
sys.path.insert(0, PYDIR)
SEED = int(os.environ.get("T2R_TEST_SEED", "0") or 0)
RNG = random.Random(SEED)


def _token_hex(n=None):
    return bytes(RNG.getrandbits(8) for _ in range(n)).hex()


secrets.token_hex = _token_hex

T = [0.0]
LOGS = []


def now():
    return T[0]


def clean(s):
    return "".join(" " if (ord(c) < 32 or c in "\x7f\x85  ") else c for c in s)


def log(*a):
    LOGS.append(clean(" ".join(str(x) for x in a)))


def emit(i, r, s):
    logs = LOGS[:]
    del LOGS[:]
    sys.stdout.write(json.dumps({"i": i, "l": logs, "r": r, "s": s}, sort_keys=True, separators=(",", ":")) + "\n")


def script():
    for line in sys.stdin:
        line = line.strip()
        if line:
            yield json.loads(line)


def tok(i):
    return "%02x" % i + "c0ffee11deadbeef"


def fl(x):
    return round(x, 2)


def main(gen, run):
    if len(sys.argv) >= 2 and sys.argv[1] == "gen":
        seed, n = int(sys.argv[2]), int(sys.argv[3])
        for c in gen(random.Random(seed * 7919 + 13), n):
            sys.stdout.write(json.dumps(c, sort_keys=True, separators=(",", ":")) + "\n")
    else:
        run()
