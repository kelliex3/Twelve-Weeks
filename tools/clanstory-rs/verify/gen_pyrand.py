import random
import sys


def main():
    meta = random.Random(20261123)
    out = sys.stdout
    out.write("# CPython %s\n" % sys.version.split()[0])
    seeds = [("int", 0), ("int", 1), ("int", 42), ("int", 2 ** 32 - 1), ("int", 2 ** 32), ("int", 2 ** 32 + 5), ("int", 2 ** 63 + 12345),
             ("int", -7), ("int", 3735928559), ("str", ""), ("str", "abc"), ("str", "3f2a1b:0:12:day5"), ("str", "jn:s:abc:1:4:new"),
             ("str", "Zażółć gęślą jaźń"), ("str", "x" * 300)]
    for i in range(25):
        seeds.append(("int", meta.getrandbits(32)))
        seeds.append(("str", "%x:%d:%d:day%d" % (meta.getrandbits(64), meta.randrange(5), meta.randrange(400), meta.randrange(90))))
    for kind, val in seeds:
        r = random.Random(val)
        if kind == "int":
            out.write("seed int %d\n" % val)
        else:
            out.write("seed str %s\n" % (val.encode().hex() or "-"))
        for _ in range(60):
            op = meta.randrange(11)
            if op == 0:
                out.write("random %r\n" % r.random())
            elif op == 1:
                k = meta.choice([0, 1, 5, 7, 31, 32, 33, 40, 63, 64])
                out.write("bits %d %d\n" % (k, r.getrandbits(k)))
            elif op == 2:
                n = meta.choice([1, 2, 3, 7, 10, 100, 255, 256, 257, 1000, 2 ** 31, 2 ** 32 + 3, 10 ** 15])
                out.write("rto %d %d\n" % (n, r.randrange(n)))
            elif op == 3:
                a = meta.randrange(-50, 50)
                b = a + meta.randrange(1, 300)
                out.write("rrange %d %d %d\n" % (a, b, r.randrange(a, b)))
            elif op == 4:
                a = meta.randrange(-50, 50)
                b = a + meta.randrange(0, 300)
                out.write("randint %d %d %d\n" % (a, b, r.randint(a, b)))
            elif op == 5:
                n = meta.randrange(1, 40)
                out.write("choice %d %d\n" % (n, r.choice(range(n))))
            elif op == 6:
                n = meta.randrange(0, 30)
                x = list(range(n))
                r.shuffle(x)
                out.write("shuffle %d %s\n" % (n, ",".join(map(str, x)) or "-"))
            elif op == 7:
                n = meta.choice([1, 3, 5, 10, 21, 22, 30, 40, 80, 200, 1000])
                k = meta.randrange(0, min(n, 40) + 1)
                out.write("sample %d %d %s\n" % (n, k, ",".join(map(str, r.sample(range(n), k))) or "-"))
            elif op == 8:
                n = meta.randrange(1, 30)
                k = meta.randrange(1, 6)
                out.write("choicesu %d %d %s\n" % (n, k, ",".join(map(str, r.choices(range(n), k=k)))))
            elif op == 9:
                n = meta.randrange(1, 12)
                if meta.random() < 0.5:
                    w = [meta.randrange(0, 20) for _ in range(n)]
                    w[-1] += 1
                else:
                    w = [round(meta.random() * 5, meta.randrange(1, 4)) for _ in range(n)]
                    w[-1] += 0.25
                k = meta.randrange(1, 4)
                out.write("choicesw %s %d %s\n" % (",".join(repr(x) for x in w), k, ",".join(map(str, r.choices(range(n), weights=w, k=k)))))
            else:
                a = meta.randrange(-40, 40)
                b = a + meta.choice([-1, 1]) * meta.randrange(1, 200)
                s = meta.choice([2, 3, 5, -2, -3, 7])
                try:
                    v = r.randrange(a, b, s)
                    out.write("rstep %d %d %d %d\n" % (a, b, s, v))
                except ValueError:
                    out.write("rstep %d %d %d err\n" % (a, b, s))


if __name__ == "__main__":
    main()
