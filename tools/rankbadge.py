"""Player rank table for the rank badge in front of every player name (ranks show in front of the names in the Lobby and on
every screen, consistently).

Op 0xBF [64B token] -> reply 0xBF text "n=<count> <HHHHHHHH:r:w> ..." : one entry per known player, keyed by a hash of the player's SHOWN name
(norm(): ASCII letters / digits only, lower case - the same on both sides, so "[TAG] Name", the vote page's "_TAG__Name" and a name cut by the
game all reach the same key), r = the chalk rank symbol 0..9, w = weeks survived in total 0..999 (tools/clan.py rank_of). The client (t2r-ext
rankbadge.c) looks every name it draws up in this table; its own rank comes from its clan state (0xE1 rk= / wk=).
The table is the same for every asker (names are public), built at most every TABLE_TTL seconds."""
import time

LIMIT = 300                  # newest players kept
TABLE_TTL = 3.0
_ALNUM = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")


def norm(name):
    """The key text of a shown name: ASCII letters / digits, lower case."""
    return "".join(ch for ch in (name or "") if ch in _ALNUM).lower()


# A name with no ASCII letter / digit (">,..,<", an all-Cyrillic or all-Japanese name) had key 0 = never listed, so it never drew a badge. Such a name
# is keyed on its other bytes instead (UTF-8, not space, not the characters the screens add or strip: "_" "[" "]" "(" ")" "<" ">"). rkb_hash does the same.
_FB_SKIP = set(b" _[]()<>")


def key(name):
    """32-bit FNV-1a of norm(name); for a name with no letters / digits, of its fallback bytes (see _FB_SKIP); 0 = nothing to key on (never listed).
    rankbadge.c rkb_hash is the same function."""
    s = norm(name)
    data = s.encode("ascii") if s else bytes(b for b in (name or "").encode("utf-8", "replace") if b > 0x20 and b not in _FB_SKIP)
    if not data:
        return 0
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def wire(rows):
    """rows = [(shown name, symbol, weeks[, role])] -> the reply text (first row wins for a duplicate key; unknown ranks (0, 0) are still listed so a
    low rank draws as rank 0 / 0 weeks, the same as the player's own page).
    An entry with a role (tools/roles.py: 1 OG, 2 admin, 3 dev, 4 lead dev) carries it as a 4th field "HHHHHHHH:r:w:role" (the badge colour,
    the lead's nametag outline). An older ext reads the first three fields and skips the rest of the word, so the field is safe to add."""
    seen, out = set(), []
    for row in rows:
        name, rk, wk = row[0], row[1], row[2]
        role = int(row[3]) if len(row) > 3 and row[3] else 0
        k = key(name)
        if not k or k in seen:
            continue
        seen.add(k)
        e = "%08x:%d:%d" % (k, max(0, min(9, int(rk))), max(0, min(999, int(wk))))
        if 0 < role <= 4:
            e += ":%d" % role
        out.append(e)
    return ("n=%d " % len(out)) + " ".join(out)


_cache = {"t": 0.0, "text": "n=0"}


def table_text(fetch, now=None):
    """fetch() -> [(shown name, symbol, weeks)]; cached TABLE_TTL s."""
    now = time.monotonic() if now is None else now
    if _cache["t"] and now - _cache["t"] < TABLE_TTL:
        return _cache["text"]
    _cache.update(t=now, text=wire(fetch()))
    return _cache["text"]
