import random
import struct
import sys


def main():
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    r = random.Random(seed)
    out = sys.stdout
    st = {"t": 1_000_000.0 + r.random(), "tick": 0.0}
    st["tick"] = st["t"] + 10
    clients = ["10.0.%d.%d:%d" % (r.randrange(4), r.randrange(1, 250), r.randrange(1024, 65535)) for _ in range(50)]
    sids = [r.getrandbits(32) for _ in range(6)]
    owners = {}

    def p(addr, data):
        out.write("P %r %s %s\n" % (st["t"], addr, data.hex() if data else "-"))

    def ka(addr):
        p(addr, b"\x03" + struct.pack("<I", r.getrandbits(32)) + bytes(r.randrange(0, 40)))

    def data_pkt(sid, src, dst, n):
        body = b"T2RO" + struct.pack("<H", r.randrange(65536)) + bytes([r.randrange(8)]) if r.random() < 0.7 else b""
        body += bytes(r.randrange(256) for _ in range(n))
        return b"\x02" + struct.pack("<I", sid) + bytes([src, dst]) + body

    phases = [("normal", 4000), ("junk", 800), ("spray", 1500), ("relayspray", 4600), ("flood", 3000), ("gap", 400), ("normal", 2000)]
    for phase, n in phases:
        for _ in range(n):
            st["t"] += r.expovariate(5000.0 if phase == "flood" else 80.0)
            if phase == "gap" and r.random() < 0.02:
                st["t"] += r.choice([16.0, 61.0, 130.0, 601.0])
            while st["t"] >= st["tick"]:
                out.write("T %r\n" % st["tick"])
                st["tick"] += 10
            x = r.random()
            if phase in ("normal", "gap"):
                if x < 0.55:
                    ka(r.choice(clients))
                elif x < 0.62:
                    sid = r.choice(sids)
                    slot = r.randrange(8)
                    a = r.choice(clients)
                    owners[(sid, slot)] = a
                    p(a, b"\x00" + struct.pack("<I", sid) + bytes([slot]) + bytes(r.randrange(0, 3)))
                elif x < 0.92 and owners:
                    (sid, src), a = r.choice(sorted(owners.items()))
                    if r.random() < 0.05:
                        a = r.choice(clients)
                    dst = 0xFF if r.random() < 0.1 else r.randrange(9)
                    p(a, data_pkt(sid, src, dst, r.randrange(0, 1300)))
                elif x < 0.95:
                    out.write("%s %s\n" % (r.choice("FU"), r.choice(clients)))
                else:
                    p(r.choice(clients), bytes([r.choice([1, 4, 5, 0x10, 0xff, 3, 0, 2])]) + bytes(r.randrange(0, 6)))
            elif phase == "junk":
                a = r.choice(clients)
                k = r.random()
                if k < 0.2:
                    p(a, b"")
                elif k < 0.4:
                    p(a, bytes(r.randrange(2049, 3000)))
                elif k < 0.6:
                    p(a, bytes([3]) + bytes(r.randrange(0, 4)))
                elif k < 0.8:
                    p(a, bytes([r.randrange(256)]) + bytes(r.randrange(0, 1400)))
                else:
                    ka(a)
            elif phase == "spray":
                a = "10.9.%d.%d:%d" % (r.randrange(256), r.randrange(256), r.randrange(1, 65535))
                if x < 0.7:
                    ka(a)
                else:
                    p(a, bytes([0x42]) + bytes(r.randrange(0, 20)))
            elif phase == "relayspray":
                p(r.choice(clients), b"\x00" + struct.pack("<I", r.getrandbits(32)) + bytes([r.randrange(256)]))
            elif phase == "flood":
                a = clients[0] if x < 0.8 else r.choice(clients)
                p(a, data_pkt(r.choice(sids), r.randrange(8), r.randrange(8), r.randrange(0, 2000)))
    out.write("T %r\n" % st["tick"])


if __name__ == "__main__":
    main()
