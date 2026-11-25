import collections
import os
import struct
import sys
import time

_CTRL = {i: " " for i in range(32)}
_CTRL.update({0x7f: " ", 0x2028: " ", 0x2029: " ", 0x85: " "})


def load(daemon_py):
    src = open(daemon_py, encoding="utf-8").read().replace("\r\n", "\n")
    a = src.index("\n# ---------------------------------------------------------------- UDP\n")
    b = src.index("\nUDP = None\n", a)
    out = []
    ns = {}

    def log(*x):
        out.append(" ".join(str(v) for v in x).translate(_CTRL))

    class Pres:
        def ping(self, addr):
            pass

    ns.update(os=os, struct=struct, time=time, collections=collections, log=log,
              evt=lambda kind, *x: log("EVT", kind, *x), RELAY={}, RELAY_TS={}, UDP_ERRS=[0],
              RELAY_STATS=collections.Counter(), RELAY_TYPES=collections.Counter(), UDP_STATS=collections.Counter(),
              PRES=Pres(), peer_seen=lambda ip, addr: None, match_sid=lambda pl: 0, MM=None)
    exec(compile(src[a:b], daemon_py + ":UDP", "exec"), ns)
    return ns, out


def take(counter):
    items = list(counter.items())
    counter.clear()
    items.sort(key=lambda kv: -kv[1])
    return items


def tick(ns, udp, log):
    STATS_LINES = 24
    if udp.pk_in:
        log("UDP-HEALTH", udp.health_line())
    if udp.counters:
        log("UDP-DROPS", dict(udp.counters)); udp.counters.clear()
    UDP_STATS, RELAY_STATS, RELAY_TYPES = ns["UDP_STATS"], ns["RELAY_STATS"], ns["RELAY_TYPES"]
    if UDP_STATS:
        snap = take(UDP_STATS)
        log("UDP-STATS", " | ".join(f"{ad[0]}:{ad[1]} op0x{op:02x}:{c}" for (ad, op), c in snap[:STATS_LINES]) + (" | ... +%d more" % (len(snap) - STATS_LINES) if len(snap) > STATS_LINES else ""))
    if RELAY_STATS:
        snap = take(RELAY_STATS)
        log("RELAY-STATS", " | ".join(f"{sid:08x} {a}->{b}:{c}" for (sid, a, b), c in snap[:STATS_LINES]) + (" | ... +%d more" % (len(snap) - STATS_LINES) if len(snap) > STATS_LINES else ""))
    if RELAY_TYPES:
        snap = take(RELAY_TYPES)
        log("RELAY-TYPES", " | ".join(f"{a}->{b} t{t:02x} {n}B:{c}" for (a, b, t, n), c in snap[:STATS_LINES]) + (" | ... +%d more" % (len(snap) - STATS_LINES) if len(snap) > STATS_LINES else ""))


def addr_of(s):
    ip, port = s.rsplit(":", 1)
    return (ip, int(port))


def main():
    ns, logs = load(sys.argv[1])
    failing = set()
    sends = []

    def send(data, addr):
        if addr in failing:
            raise OSError(101, "Network is unreachable")
        sends.append("S %s:%d %s" % (addr[0], addr[1], data.hex()))

    udp = ns["UdpRelay"](send)
    udp.sock_stats = "sockbuf rcv 0 KB snd 0 KB"
    log = ns["log"]
    w = sys.stdout
    for line in sys.stdin:
        f = line.rstrip("\n").split(" ")
        if f[0] == "P":
            udp.handle(b"" if f[3] == "-" else bytes.fromhex(f[3]), addr_of(f[2]), float(f[1]))
        elif f[0] == "T":
            tick(ns, udp, log)
        elif f[0] == "F":
            failing.add(addr_of(f[1]))
        elif f[0] == "U":
            failing.discard(addr_of(f[1]))
        for s in sends:
            w.write(s + "\n")
        for l in logs:
            w.write("L " + l + "\n")
        sends.clear()
        logs.clear()


if __name__ == "__main__":
    main()
