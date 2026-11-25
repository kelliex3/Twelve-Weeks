"""live_counts.py: counts + channel names from a daemon snapshot (no Discord). python test_live_counts.py [real t2r-status.json]"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_counts as lc

NOW = 1795268266.0


def snap(players, matches, ts=NOW):
    return {"ts": ts, "players_online": len(players), "live": {"players": players, "matches": matches}}


def p(state, match=None):
    return {"name": "x", "state": state, "match": match}


def main():
    fails = 0

    def check(ok, what):
        nonlocal fails
        print(("ok   " if ok else "FAIL ") + what)
        fails += 0 if ok else 1

    s = snap([p("match", "a"), p("match", "a"), p("match", "b"), p("match", "c"), p("match", "d"), p("queue"), p("menu"), p("menu")],
             [{"id": "a", "state": "running", "game_mode": "supply-raid"}, {"id": "b", "state": "running", "game_mode": "interrogation"},
              {"id": "c", "state": "loading", "game_mode": "interrogation"}, {"id": "d", "state": "ended", "game_mode": "supply-raid"}])
    c = lc.counts(s, {"ts": NOW, "open": 12}, now=NOW + 5)
    check(c == {"supply-raid": 2, "interrogation": 1, "lobby": 2, "menus": 3, "launcher": 12, "online": 8}, "mixed modes: %s" % c)
    n = dict(lc.names(c))
    check(n["supply-raid"] == "Supply Raid: 2 in match" and n["launcher"] == "Launcher open: 12", "names: %s" % n)

    old = snap([p("match", "a")], [{"id": "a", "state": "running", "mode": ""}])            # daemon without game_mode = Supply Raid
    check(lc.counts(old, None, now=NOW)["supply-raid"] == 1, "no game_mode -> Supply Raid")

    st = lc.counts(snap([p("menu")], [], ts=NOW - 600), {"ts": NOW - 600, "open": 3}, now=NOW)
    check(st["supply-raid"] is None and st["launcher"] is None, "stale sources -> None")
    check(dict(lc.names(st))["interrogation"] == "Interrogation: offline", "stale -> offline name")
    check(lc.counts(None, None, now=NOW)["menus"] is None, "missing files -> None")
    check(all(len(x) <= 100 for _, x in lc.names({"supply-raid": 10 ** 120})), "names capped at 100 chars")

    if len(sys.argv) > 1:                       # a real snapshot copied from the server
        real = json.load(open(sys.argv[1], encoding="utf-8"))
        rc = lc.counts(real, None, now=real["ts"])
        live = real["live"]
        check(sum(rc[k] for k in ("supply-raid", "interrogation", "lobby", "menus")) == len(live["players"]),
              "real snapshot: every player counted once %s" % rc)
        for k, x in lc.names(rc):
            print("     %-14s %s" % (k, x))
    print("%d failure(s)" % fails)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
