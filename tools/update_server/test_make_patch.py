"""make_patch.py on small synthetic packs: the plan + patch file rebuild the new file exactly (reference apply below, same rules as
release/launcher/lib/update/patch.js), for a data-only pack and a pack split into t2r-data.bin + part1.dat. Needs numpy (t2r_pack)."""
import hashlib
import os
import random
import struct
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
try:
    import numpy as np  # noqa: F401
    import t2r_pack as tp
    import make_patch as mp
except ImportError as e:                      # (the server's stdlib-only test run)
    tp = None
    _why = str(e)


def ks_bytes(pos, n):
    q0, q1 = pos // 8, (pos + n + 7) // 8
    k = tp._keystream_chunk(np.zeros(q1 - q0, dtype="<u8"), q0).tobytes()
    return k[pos - q0 * 8:pos - q0 * 8 + n]


def build_stream(chunks):
    """plain DSAR: header 0x28, entries 0x20 each, then the chunks (8-aligned)"""
    n = len(chunks)
    data_start = 0x28 + n * 0x20
    off = (data_start + 7) // 8 * 8
    ents, body = [], bytearray()
    for c in chunks:
        ents.append((off + len(body), len(c)))
        body += c
        body += b"\0" * ((-len(body)) % 8)
    out = bytearray(struct.pack("<4sIIIQ", b"DSAR", 1, n, data_start, 0) + b"\0" * 0x10)
    for eo, csz in ents:
        out += struct.pack("<QIIQQ", eo, csz * 2, csz, 0x5555555555555503, 0)
    out += b"\0" * (off - len(out))
    out += body
    return bytes(out), ents


def write_pack(d, name, chunks, side_from=None):
    """-> (data path, part1 path or None). side_from = index of the first chunk that goes to part1.dat"""
    plain, ents = build_stream(chunks)
    enc = bytes(tp.keystream_xor(plain))
    cut = ents[side_from][0] if side_from is not None else len(enc)
    assert cut % 8 == 0
    data = os.path.join(d, name + "-t2r-data.bin")
    hdr = bytearray(mp.DATA_HDR)
    hdr[:4] = b"T2RD"
    struct.pack_into("<IQ", hdr, 4, 2, cut)
    with open(data, "wb") as f:
        f.write(bytes(hdr) + enc[:cut] + os.urandom(16))
    p1 = None
    if side_from is not None:
        p1 = os.path.join(d, name + "-part1.dat")
        with open(p1, "wb") as f:
            f.write(enc[cut:] + os.urandom(16))
    return data, p1


def apply_ref(old, patch, out):
    with open(patch, "rb") as f:
        b = f.read()
    h = mp.HDR.unpack_from(b, 0)
    sv, dv, nops = h[6:9], h[9:12], h[12]
    src = open(old, "rb").read()
    o = bytearray()
    p = mp.HDR.size

    def mask(view, at, n):
        off, ln, pos = view
        lo, hi = max(at, off), min(at + n, off + ln // 8 * 8)
        m = bytearray(n)
        if hi > lo:
            m[lo - at:hi - at] = ks_bytes(pos + lo - off, hi - lo)
        return m

    for _ in range(nops):
        kind, _z, n = mp.OP.unpack_from(b, p)
        p += mp.OP.size
        if kind == mp.RAW:
            o += b[p:p + n]
            p += n
        elif kind == mp.RAWZ:
            zl = struct.unpack_from("<Q", b, p)[0]
            p += 8
            z = zlib.decompress(b[p:p + zl])
            assert len(z) == n
            o += z
            p += zl
        else:
            so = struct.unpack_from("<Q", b, p)[0]
            p += 8
            piece = bytes(src[so:so + n])
            ms, md = mask(sv, so, n), mask(dv, len(o), n)
            o += bytes(x ^ y ^ z for x, y, z in zip(piece, ms, md))
    assert p == len(b)
    open(out, "wb").write(o)


@unittest.skipIf(tp is None, "numpy / t2r_pack not available")
class MakePatch(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="t2pt-")
        rnd = random.Random(7)
        self.chunks = [bytes(rnd.getrandbits(8) for _ in range(rnd.randint(40, 900))) for _ in range(40)]

    def new_chunks(self):
        c = list(self.chunks)
        c.insert(3, os.urandom(333))          # everything after it moves
        c[10] = os.urandom(len(c[10]) + 5)    # one changed
        del c[20]                             # one dropped
        c.append(self.chunks[5])              # one repeated
        return c

    def check(self, old, new, patch):
        out = os.path.join(self.d, "out.bin")
        apply_ref(old, patch, out)
        self.assertEqual(hashlib.sha256(open(out, "rb").read()).hexdigest(), hashlib.sha256(open(new, "rb").read()).hexdigest())
        h = mp.read_header(patch)
        self.assertEqual(h["to"], hashlib.sha256(open(new, "rb").read()).hexdigest())
        self.assertEqual(h["from"], hashlib.sha256(open(old, "rb").read()).hexdigest())

    def test_data_only(self):
        od, _ = write_pack(self.d, "old", self.chunks)
        nd, _ = write_pack(self.d, "new", self.new_chunks())
        out = os.path.join(self.d, "p.t2pt")
        info = mp.make("data", [od, nd], out)
        self.check(od, nd, out)
        self.assertGreater(info["copied"], (info["literal"] - mp.DATA_HDR - 16) // 2)   # most chunks are copied, not sent (raw counts the 64 KiB header)

    def test_split_part1(self):
        od, op = write_pack(self.d, "old", self.chunks, side_from=25)
        nd, np1 = write_pack(self.d, "new", self.new_chunks(), side_from=26)
        out = os.path.join(self.d, "p1.t2pt")
        info = mp.make("part1", [od, op, nd, np1], out)
        self.check(op, np1, out)
        self.assertGreater(info["copied"], 0)
        outd = os.path.join(self.d, "pd.t2pt")
        mp.make("data", [od, nd], outd)
        self.check(od, nd, outd)

    def test_plain_file(self):
        rnd = random.Random(3)
        base = bytearray(rnd.getrandbits(8) for _ in range(300000))
        base[100000:140000] = b"code " * 8000                      # compressible part
        old = os.path.join(self.d, "old.dll")
        new = os.path.join(self.d, "new.dll")
        open(old, "wb").write(base)
        nb = bytearray(base)
        nb[5000:5010] = b"0123456789"                              # small edit in place
        nb[200000:200000] = os.urandom(777)                        # insertion: the rest moves
        del nb[250000:251000]                                      # deletion
        nb += base[0:20000]                                        # a moved copy of the start
        open(new, "wb").write(nb)
        out = os.path.join(self.d, "f.t2pt")
        info = mp.make("file", [old, new], out)
        self.check(old, new, out)
        self.assertLess(info["size"], 40000)                       # far below the 300 KB file

    def test_install(self):
        import json
        od, _ = write_pack(self.d, "old", self.chunks)
        nd, _ = write_pack(self.d, "new", self.new_chunks())
        out = os.path.join(self.d, "p.t2pt")
        info = mp.make("data", [od, nd], out)
        data = os.path.join(self.d, "update-data")
        man = os.path.join(self.d, "m.json")
        json.dump({"files": [{"path": "t2r-maps/main/t2r-data.bin", "sha256": info["to"], "size": os.path.getsize(nd)},
                             {"path": "x.txt", "sha256": "0" * 64, "size": 1}]}, open(man, "w"))
        mp.install(data, man, out)
        mp.install(data, man, out)                              # twice: still one entry
        m = json.load(open(man))
        self.assertEqual(m["files"][0]["patches"], [{"from": info["from"], "sha256": info["sha256"], "size": info["size"]}])
        self.assertNotIn("patches", m["files"][1])
        self.assertEqual(os.path.getsize(os.path.join(data, "blobs", info["sha256"])), info["size"])


if __name__ == "__main__":
    unittest.main()
