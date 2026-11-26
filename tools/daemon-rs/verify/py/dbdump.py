#!/usr/bin/env python3
"""Deterministic text dump of a daemon database: python3 dbdump.py <t2r.db>
Tables in name order, rows sorted by their text form, BLOBs as hex, every epoch-looking number (also inside text / JSON values) as <T>."""
import re, sqlite3, sys

EPOCH = re.compile(r"(?<![0-9.])1[4-9][0-9]{8}(\.[0-9]+)?(?![0-9])")

def cell(v):
    if v is None:
        return "NULL"
    if isinstance(v, (bytes, bytearray)):
        return "x'" + bytes(v).hex() + "'"
    if isinstance(v, float):
        return "<T>" if 1.4e9 < v < 2.6e9 else repr(v)
    if isinstance(v, int):
        return "<T>" if 1_400_000_000 < v < 2_600_000_000 else str(v)
    return "'" + EPOCH.sub("<T>", str(v)).replace("\n", "\n") + "'"

def main(path):
    c = sqlite3.connect("file:%s?mode=ro" % path, uri=True)
    tabs = [r[0] for r in c.execute("select name from sqlite_master where type='table' order by name")]
    for t in tabs:
        cols = [r[1] for r in c.execute('pragma table_info("%s")' % t)]
        rows = sorted(" | ".join(cell(v) for v in r) for r in c.execute('select * from "%s"' % t))
        print("TABLE %s (%s) rows=%d" % (t, ",".join(cols), len(rows)))
        for r in rows:
            print("  " + r)
    idx = sorted(r[0] for r in c.execute("select name from sqlite_master where type='index' and name not like 'sqlite_%'"))
    print("INDEXES " + ",".join(idx))

if __name__ == "__main__":
    main(sys.argv[1])
