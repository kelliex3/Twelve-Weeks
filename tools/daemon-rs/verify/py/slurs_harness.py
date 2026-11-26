"""Differential test of tools/slurs.py (see cseed.py): find() / _words() on generated chat lines, load_extra() on generated files, and a sweep
of every code point (a sha256 per range) through _words()."""
import hashlib
import os
import shutil
import tempfile

import cseed
from cseed import PYDIR, emit, main, script

BAD = ["nigger", "niggers", "n1gg3r", "n!gger", "nigga", "kike", "kike!", "chink", "spic", "gook", "faggot", "fag", "coon", "paki", "tranny", "wetback",
       "porch monkey", "porchmonkey", "sand nigger", "zipperhead", "spearchucker", "nignog", "xXniggaXx", "dothead", "camelfucker", "n i g g e r", "f a g g o t",
       "n.i.g.g.e.r", "ni gger", "nig ger", "fa g g o t", "k i k e", "beaner", "raghead", "towelhead", "junglebuny", "jigabo", "wog", "golliwog", "chinkie"]
CLEAN = ["hello there", "Nigeria", "snigger", "niggle", "spice girls", "skiing", "minigame", "raccoon", "negroni", "beanery", "fagot", "go ok", "mini gerbil",
         "", "   ", "!!!", "don't shoot me", "gg wp", "ez", "good game", "TLOU2 rocks", "i am from Niger", "coonskin cap", "Scunthorpe", "kikeriki", "pakistan"]
FANCY = ["ｎｉｇｇｅｒ", "𝐧𝐢𝐠𝐠𝐞𝐫", "ⓝⓘⓖⓖⓔⓡ", "ni\u0301gger", "n\u200bigger", "nіgger", "nιgger", "ΝΙGGER", "ｋｉｋｅ！", "KIKE！", "kıke", "ﬁg", "n\u00a0igger", "porch\u3000monkey",
         "ni\u0307gg\u0301er", "\u212aike", "k\u0131ke", "ǆ", "Ⅳ", "ｆａｇｇｏｔ", "ᴋɪᴋᴇ", "nigger\u200b", "n\u00adigger", "\u00a0kike\u00a0", "!kike!", "!!kike!!", "!х!kike", "kike!б"]
EXTRA = ["", "# only a comment", "testword", "*longwordx", "*Ünïcode", "Wörd", "a", "*abc", "*Ab.Cd1", "TESTWORD # c", "  spaced  ", "*niger", "*\u0430\u0431\u0432\u0433\u0434",
         "w0rd5", "Ж", "*ǅwordˇ", "x" * 5]


def rand_text(r):
    k = r.random()
    if k < 0.25:
        w = r.choice(BAD)
    elif k < 0.40:
        w = r.choice(CLEAN)
    elif k < 0.55:
        w = r.choice(FANCY)
    elif k < 0.75:
        w = "".join(chr(r.choice([r.randrange(0x20, 0x7F), r.randrange(0x80, 0x250), r.randrange(0x370, 0x500), r.randrange(0x2000, 0x2200), r.randrange(0xFF00, 0xFFF0),
                                  r.randrange(0x1D400, 0x1D800), r.randrange(0x300, 0x370), r.randrange(0x10000, 0x10FFFF) if r.random() < 0.3 else r.randrange(0x20, 0x7F)]))
                    for _ in range(r.randint(1, 12)))
        w = "".join(ch for ch in w if not 0xD800 <= ord(ch) <= 0xDFFF)
    else:
        w = ""
    parts = [w]
    if r.random() < 0.5:
        parts.insert(0, r.choice(CLEAN + ["", "u", "the", "i hate", "ok"]))
    if r.random() < 0.4:
        parts.append(r.choice(CLEAN + BAD + [""]))
    s = r.choice([" ", " ", "  ", "\t", "\u00a0", "\n"]).join(parts)
    if r.random() < 0.3:
        s = r.choice(["!", "!!", "*", "", "x"]) + s + r.choice(["!", "", "x", "!!"])
    if r.random() < 0.15:     # a word with junk between its letters
        s = "".join(ch + r.choice(["", "", ".", "-", "_", "\u0301", "1", "!", "\u200b"]) for ch in s)
    if r.random() < 0.1:
        s = s.upper()
    return s


def gen(r, n):
    for i in range(n):
        x = r.random()
        if x < 0.6:
            yield {"op": "find", "text": rand_text(r)}
        elif x < 0.8:
            yield {"op": "words", "text": rand_text(r)}
        elif x < 0.9:
            lines = r.sample(EXTRA, r.randint(0, 5))
            sep = r.choice(["\n", "\r\n", "\r"])
            raw = sep.join(lines).encode("utf-8")
            if r.random() < 0.2:
                raw += bytes(r.randrange(256) for _ in range(r.randint(1, 4)))
            yield {"op": "extra", "hex": raw.hex(), "missing": r.random() < 0.1}
        else:
            yield {"op": "find", "text": r.choice(BAD) + " " + rand_text(r)}
    # a few ranges of code points; seed 1 covers everything
    for lo in range(0, 0x110000, 0x10000):
        yield {"op": "sweep", "lo": lo, "hi": lo + 0x10000}
    yield {"op": "find", "text": "nigger"}
    yield {"op": "extra", "hex": "".encode().hex(), "missing": True}


def run():
    import slurs
    d = tempfile.mkdtemp(prefix="slurs_py")
    p = os.path.join(d, "slurs.txt")
    for i, c in enumerate(script()):
        op = c["op"]
        r = None
        s = None
        if op == "find":
            r = slurs.find(c["text"])
        elif op == "words":
            r = slurs._words(c["text"])
        elif op == "extra":
            if c.get("missing"):
                try:
                    os.remove(p)
                except OSError:
                    pass
            else:
                with open(p, "wb") as f:
                    f.write(bytes.fromhex(c["hex"]))
            r = slurs.load_extra(p)
            s = {"any": list(slurs._any), "word": sorted(slurs._word)}
        elif op == "sweep":
            h = hashlib.sha256()
            for cp in range(c["lo"], c["hi"]):
                if 0xD800 <= cp <= 0xDFFF:
                    continue
                ch = chr(cp)
                a = slurs._words("a%sb %s" % (ch, ch))
                b = slurs._words("!%s!kike!%s x%s!" % (ch, ch, ch))
                h.update(("%x:%s|%s\n" % (cp, ",".join(a), ",".join(b))).encode("utf-8"))
            r = h.hexdigest()
        emit(i, r, s)
    shutil.rmtree(d, ignore_errors=True)


main(gen, run)
