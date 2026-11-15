"""Resolve TLOU2 string ids (FNV-1a-64) using the game's sidbase.bin.
  python sid.py <file_with_hashes...>     # resolves every 0x... 64-bit constant found
  python sid.py -s <string>               # hash a string
"""
import re, struct, sys, os
_GAMES = r"C:\Games"
SIDBASE_CANDIDATES = [os.path.join(_GAMES, d, "tlou-ii-mp", "sidbase.bin") for d in ("The Last of Us Part II Remastered", "T2R-A", "T2R-B", "bk")] + [
    os.path.join(_GAMES, "The Last of Us Part II Remastered", d, "tlou-ii-mp", "sidbase.bin") for d in ("TwelveWeeks-disabled", "Factions2-disabled")]


def find_sidbase(extra=()):
    """T2R_SIDBASE, then the given paths, then the usual game folders (the main game's tlou-ii-mp may be moved away: the A/B folders have it)."""
    for c in [os.environ.get("T2R_SIDBASE")] + list(extra) + SIDBASE_CANDIDATES:
        if c and os.path.isfile(c):
            return c
    return SIDBASE_CANDIDATES[0]


SIDBASE = find_sidbase()

def fnv(s):
    h = 0xCBF29CE484222325
    for c in s.encode():
        h = ((h ^ c) * 0x100000001B3) & (2**64 - 1)
    return h

def load():
    d = open(SIDBASE, "rb").read()
    n = struct.unpack_from("<Q", d, 0)[0]
    arr = memoryview(d)[8:8 + n * 16].cast("Q")
    table = {}
    for i in range(n):
        h, off = arr[2 * i], arr[2 * i + 1]
        table[h] = off
    return d, table

if __name__ == "__main__":
    if sys.argv[1] == "-s":
        for s in sys.argv[2:]: print(f"{fnv(s):#018x} {s}")
        sys.exit()
    d, t = load()
    seen = set()
    for f in sys.argv[1:]:
        for m in re.finditer(r"0x[0-9a-fA-F]{12,16}|-0x[0-9a-fA-F]{12,16}", open(f, encoding="utf-8", errors="ignore").read()):
            v = int(m.group(), 16) & (2**64 - 1)
            if v in seen: continue
            seen.add(v)
            if v in t:
                off = t[v]; e = d.index(b"\0", off)
                print(f"{v:#018x} {d[off:e].decode('latin1')}")
