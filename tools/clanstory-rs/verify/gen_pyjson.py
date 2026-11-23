import json
import random
import struct
import sys
import zlib


def rnd_str(r):
    pool = ["a", "Z", " ", "\"", "\\", "/", "\n", "\t", "\r", "\b", "\f", "\x01", "\x1f", "\x7f", "é", "ż", "ł", "€", " ",
            "😀", "日本", "|", "{A}", "x"]
    return "".join(r.choice(pool) for _ in range(r.randrange(0, 12)))


def rnd_float(r):
    k = r.randrange(9)
    if k == 0:
        return r.choice([0.0, -0.0, 1.0, -1.0, 0.1, 0.5, 1e16, 1e15, 9999999999999998.0, 1e-4, 1e-5, 0.0001234, 123456789.125,
                         5e-324, 1.7976931348623157e308, 2.5, 1e22, 1e-7, 100.0, 1234567890123456.7])
    if k == 1:
        return struct.unpack("<d", struct.pack("<Q", r.getrandbits(64) & 0x7FEFFFFFFFFFFFFF))[0]
    if k == 2:
        return round(r.random() * 1000, r.randrange(0, 4))
    if k == 3:
        return r.random() * 10 ** r.randrange(-8, 20)
    if k == 4:
        return float(r.randrange(-10 ** 6, 10 ** 6)) / r.choice([1, 2, 4, 8, 10, 100, 1000, 3])
    if k == 5:
        return 1752345678.0 + round(r.random() * 1000, 3)
    if k == 6:
        return struct.unpack("<f", struct.pack("<f", r.random() * 100))[0]
    if k == 7:
        return -r.random() * 10 ** r.randrange(-3, 17)
    return r.random()


def rnd_val(r, depth=0):
    k = r.randrange(9 if depth < 3 else 6)
    if k == 0:
        return None
    if k == 1:
        return r.random() < 0.5
    if k == 2:
        return r.choice([0, 1, -1, 7, 2 ** 31, 2 ** 53 + 1, -(2 ** 63), 2 ** 63 - 1, r.randrange(-10 ** 9, 10 ** 9)])
    if k == 3:
        return rnd_float(r)
    if k in (4, 5):
        return rnd_str(r)
    if k in (6, 7):
        return {rnd_str(r) or "k": rnd_val(r, depth + 1) for _ in range(r.randrange(0, 5))}
    return [rnd_val(r, depth + 1) for _ in range(r.randrange(0, 5))]


def main():
    r = random.Random(77)
    out = sys.stdout
    out.write("# CPython %s\n" % sys.version.split()[0])
    for _ in range(3000):
        v = rnd_val(r)
        out.write("V %s\n" % json.dumps(v, separators=(",", ":")))
        out.write("D %s\n" % json.dumps(v))
        out.write("U %s\n" % json.dumps(v, separators=(",", ":"), ensure_ascii=False).encode("utf-8").hex())
    for _ in range(3000):
        f = rnd_float(r)
        out.write("F %s %s\n" % (struct.pack(">d", f).hex(), repr(f)))
    for _ in range(500):
        s = rnd_str(r)
        parts = [rnd_str(r) for _ in range(r.randrange(1, 5))]
        out.write("C %s %d\n" % (s.encode("utf-8").hex() or "-", zlib.crc32(s.encode("utf-8"))))
        out.write("H %s %d\n" % (",".join(p.encode("utf-8").hex() or "-" for p in parts), zlib.crc32("|".join(parts).encode("utf-8"))))


if __name__ == "__main__":
    main()
