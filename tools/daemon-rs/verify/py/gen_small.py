#!/usr/bin/env python3
"""Script generator for the small modules (names, notices, roles, rankbadge): gen_small.py <seed> [calls] > script.jsonl
The lines of the four modules are mixed; the harness of each module takes its own ("m")."""
import json
import random
import sys

seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
N = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
R = random.Random(seed)
T = [1_700_000_000.0]
OUT = []


def emit(m, op, **kw):
    d = {"m": m, "op": op, "t": round(T[0], 3)}
    d.update(kw)
    OUT.append(json.dumps(d, separators=(",", ":")))


def advance():
    r = R.random()
    if r < 0.6:
        T[0] += R.random() * 5
    elif r < 0.9:
        T[0] += R.random() * 120
    elif r < 0.98:
        T[0] += 100 + R.random() * 4000
    else:
        T[0] += 86400 * R.random() * 3


NAME_BITS = ["Ellie", "joel", "A b", "  Abby  ", "Dina  Fox", "x" * 15, "x" * 16, "_bad", "ok_name", "Zoë", "a.b-c_d", "", " ", "Tom my", "9lives",
             "Łukasz", "İstanbul", "a\tb", "a\nb", "-dash", "name!", "N" * 40, "ab  cd   ef", " nb ", "　x　", "X́", "dot.", "sp ace "]
TAG_BITS = ["", "ab", "TAG", "tag12", "toolong", "t g", "ß", "ſ", "a-b", "  xy ", "12345", "123456", "ı", "É", "abc", "ABCDE"]
HEXC = "0123456789abcdefABCDEF"


def tok():
    r = R.random()
    if r < 0.5:
        return "".join(R.choice(HEXC) for _ in range(64))
    if r < 0.7:
        return "".join(R.choice("0123456789abcdef") for _ in range(24))
    if r < 0.85:
        return R.choice(["", "ABC", "İ" * 64, "x" * 64, "A" * 63, "İ" * 32])
    return "".join(R.choice(HEXC) for _ in range(R.randrange(0, 90)))


tids = ["a" * 24, "b" * 24, "s:76561190000000001", "s:76561190000000002", "c" * 24, "tid x", "T2N"]


def names_op():
    r = R.random()
    if r < 0.12:
        emit("names", "token_id", token=tok())
    elif r < 0.3:
        emit("names", "clean", name=R.choice(NAME_BITS), tag=R.choice(TAG_BITS))
    elif r < 0.4:
        emit("names", "load", tid=R.choice(tids))
    elif r < 0.8:
        kw = dict(tid=R.choice(tids), name=R.choice(NAME_BITS), tag=R.choice(TAG_BITS))
        if R.random() < 0.5:
            kw["taken"] = R.sample(NAME_BITS, R.randrange(0, 6)) + ["ok_name"] * R.randrange(2)
        emit("names", "save", **kw)
    elif r < 0.9:
        emit("names", "display", name=R.choice(NAME_BITS), tag=R.choice(TAG_BITS))
    else:
        kw = dict(name=R.choice(NAME_BITS), tag=R.choice(TAG_BITS))
        if R.random() < 0.5:
            kw["err"] = R.choice(["name taken", "x y z", "", "bad"])
        emit("names", "state_text", **kw)


TEXTS = ["hello", "a|b|c", "tab\there", "x" * 200, "Cześć żółw", "", "  pad  ", "ctrl\x00\x01\x1f\x7f", "l s e\x85", "​zw",
         "line1\nline2", "restart in 5 minutes", "maintenance – back soon"]
ids = []
KINDS = ["u", "v", "r", "s", "t", "p"]


def notices_op():
    r = R.random()
    if r < 0.25:
        kw = dict(text=R.choice(TEXTS))
        if R.random() < 0.5:
            kw["minutes"] = R.choice([0, 0.1, 0.5, 1, 30, 1000, 5000, -3, 2.5])
        if R.random() < 0.5:
            kw["seconds"] = R.choice([0, 1, 2, 5, 5.5, 20, 600, 601, 9999, -5])
        if R.random() < 0.5:
            kw["text_pl"] = R.choice(TEXTS)
        emit("notices", "add_text", **kw)
    elif r < 0.33:
        emit("notices", "clear", id=R.choice([R.randrange(0, 40), -1, 1, 2, 3]))
    elif r < 0.43:
        kw = dict(minutes=R.choice([0, 0.2, 1, 5, 60, 2000, -4, 2.5]), reason=R.choice(TEXTS))
        if R.random() < 0.5:
            kw["restart"] = R.random() < 0.5
        if R.random() < 0.5:
            kw["reason_pl"] = R.choice(TEXTS)
        emit("notices", "schedule", **kw)
    elif r < 0.48:
        emit("notices", "cancel")
    elif r < 0.53:
        emit("notices", "go_final", hold=R.choice([0, 1, 60, 6.5]))
    elif r < 0.58:
        emit("notices", "due")
    elif r < 0.82:
        emit("notices", "active", stale=R.random() < 0.3, refused=R.random() < 0.3, lang=R.choice(["en", "pl", "xx"]))
    elif r < 0.9:
        emit("notices", "public")
    elif r < 0.95:
        emit("notices", "clean", s=R.choice(TEXTS), n=R.choice([140, 5, 0, 50]))
    else:
        items = [[R.randrange(0, 5), R.choice(KINDS), R.randrange(0, 700), R.choice(TEXTS)] for _ in range(R.randrange(0, 5))]
        emit("notices", "reply_text", items=items)


def sid():
    return R.choice([76561190000000001, 76561190000000002, 76561190000000003, 76561190000000004, 5, 0, 76561198000000000])


def roles_line():
    r = R.random()
    kind = R.choice(["og", "admin", "dev"])
    if kind == "dev":
        return "%d %s # %s" % (sid(), R.choice(["", "lead", "LEAD", "Lead", "lead x", "notlead"]), R.choice(["", "name", "TEMP dev"]))
    if kind == "admin":
        return "%d # %s" % (sid(), R.choice(["", "temp", "TEMP", "x TEMP y", "ok"]))
    return R.choice(["%d", "  %d  ", "%d # c", "# only %d", "%d\t7", "junk %d", "-%d", "0%d", "%d.5"]) % sid() if R.random() < 0.9 else "0"


def roles_op():
    r = R.random()
    if r < 0.2:
        kind = R.choice(["og", "admin", "dev"])
        if R.random() < 0.1:
            emit("roles", "write", kind=kind, hex=None)
            return
        sep = R.choice(["\n", "\r\n", "\r", "\x0b", "\x1c", "\x85", " "])
        text = sep.join(roles_line() for _ in range(R.randrange(0, 8))) + R.choice(["", sep])
        b = text.encode("utf-8")
        if R.random() < 0.1:
            b = b + bytes([0xff, 0xfe]) + b"\n12\n"
        if R.random() < 0.05:
            b = b"\xef\xbb\xbf" + b
        emit("roles", "write", kind=kind, hex=b.hex(), mt=R.choice([1700000000 + R.randrange(100), 1700000000]))
    elif r < 0.8:
        emit("roles", "role_of", sid=sid())
    else:
        text = "\n".join(roles_line() for _ in range(R.randrange(0, 6)))
        emit("roles", "parse", text=text, skip_temp=R.random() < 0.5)


RN = ["Ellie", "ellie", "E l l i e", "[TAG] Ellie", "_TAG__Ellie", "Joel", "", "!!!", "Élie", "x" * 60, "Abby99", "ABBY99", "Dina", "Tommy"]


def rows():
    out = []
    for _ in range(R.randrange(0, 8)):
        r = [R.choice(RN), R.choice([0, 3, 9, 10, -2, 5]), R.choice([0, 12, 999, 1000, -5, 40])]
        if R.random() < 0.6:
            r.append(R.choice([0, 1, 2, 3, 4, 5, -1, None]))
        out.append(r)
    return out


def rankbadge_op():
    r = R.random()
    if r < 0.15:
        emit("rankbadge", "norm", name=R.choice(RN))
    elif r < 0.3:
        emit("rankbadge", "key", name=R.choice(RN))
    elif r < 0.55:
        emit("rankbadge", "wire", rows=rows())
    elif r < 0.62:
        emit("rankbadge", "ttl", ttl=R.choice([3.0, 30.0, 0.5, 0]))
    else:
        emit("rankbadge", "table", rows=rows(), now=round(R.choice([0.0, T[0] % 1000, T[0] % 1000 + 2, 5.5]) + R.randrange(0, 60), 3))


def main():
    for _ in range(N):
        advance()
        r = R.random()
        if r < 0.25:
            names_op()
        elif r < 0.5:
            notices_op()
        elif r < 0.75:
            roles_op()
        else:
            rankbadge_op()
    print("\n".join(OUT))


main()
