import json
import os
import subprocess
import sys
import tempfile

SYNTHETIC = [
    ({"ts": 1000, "players_online": 3, "live": {"players": [
        {"state": "match", "match": "a"}, {"state": "match", "match": "a"}, {"state": "match", "match": "b"},
        {"state": "match", "match": "c"}, {"state": "match", "match": "d"}, {"state": "match", "match": "zz"},
        {"state": "queue"}, {"state": "menu"}, {"state": None}],
        "matches": [{"id": "a", "state": "running", "game_mode": "supply-raid"}, {"id": "b", "state": "running", "game_mode": "interrogation"},
                    {"id": "c", "state": "ended"}, {"id": "d", "state": "loading"}]}}, {"ts": 1, "open": 12, "window_s": 75.0}),
    ({"ts": 5, "live": {"players": [{"state": "match", "match": 7}], "matches": [{"id": 7, "state": "running"}]}}, None),
    ({"ts": 5, "players_online": 4, "live": {"players": [], "matches": []}}, {"ts": 0, "open": 0}),
    ({}, {"open": 3}),
    (None, None),
    ({"ts": 9, "live": {"players": [{"state": "match", "match": "q"}], "matches": [{"id": "q", "state": "running", "game_mode": "brawl"}]}}, {"ts": 9, "open": 2}),
]


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


if __name__ == "__main__":
    bin_, bot_dir = sys.argv[1], sys.argv[2]
    py = sys.executable
    pairs = []
    tmp = tempfile.mkdtemp()
    for i, (s, l) in enumerate(SYNTHETIC):
        sp, lp = os.path.join(tmp, "s%d.json" % i), os.path.join(tmp, "l%d.json" % i)
        if s is not None:
            json.dump(s, open(sp, "w"))
        if l is not None:
            json.dump(l, open(lp, "w"))
        pairs.append((sp, lp, "synthetic %d" % i))
    for real in sys.argv[3:]:
        pairs.append((real, os.path.join(os.path.dirname(real), "t2r-launchers.json"), real))
    bad = 0
    for sp, lp, label in pairs:
        a = run([py, os.path.join(bot_dir, "live_counts.py"), sp, lp, "--at-snapshot"])
        b = run([bin_, "live-counts", sp, lp, "--at-snapshot"])
        same = a == b
        bad += not same
        print("%s %s: %s" % ("same" if same else "DIFF", label, a.splitlines()[0]) + ("" if same else "\n--- python\n%s--- rust\n%s" % (a, b)))
    print("live counts: %d differences" % bad)
    sys.exit(1 if bad else 0)
