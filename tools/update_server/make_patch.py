"""Patch files for the two big pack files (t2r-maps/main/t2r-data.bin and the Part I side file part1.dat).

Why: both files are one encrypted stream (tools/t2r_pack.py: DSAR of LZ4 chunks, XOR'd with a keystream indexed by the stream
position). Any change in the pack moves every later byte, so the launcher's 1 MiB block reuse finds nothing and every update was the
whole 2.2 GB again. In the DECRYPTED stream most DSAR chunks are unchanged, only at other positions. A patch lists, for the new file:
  RAW  bytes  - written as they are (new chunks, the DSAR directory, headers, trailers: already encrypted where they lie in the stream)
  COPY off n  - n bytes of the installed old file from file offset `off`, decrypted with the old stream position and encrypted again
                with the new one (release/launcher/lib/update/patch.js)
so a player downloads only the changed chunks.

  python make_patch.py data  OLD_DATA NEW_DATA OUT.t2pt
  python make_patch.py part1 OLD_DATA OLD_PART1 NEW_DATA NEW_PART1 OUT.t2pt     (the data files give the stream position + directory)
  python make_patch.py file  OLD NEW OUT.t2pt     any other file (t2r-ext.dll, winmm.dll ...): plain binary, no keystream; chunks found
        by content-defined chunking (a gear hash cut, ~8 KiB average) and grown byte by byte into the changed parts around them
  python make_patch.py install --data <update-data> --manifest <manifest json> OUT.t2pt [--part1]
        copies the patch into the blob store and adds {"from", "sha256", "size"} to the manifest entry whose sha256 is the patch's target
        (light: no decrypting, safe to run on the server). --part1: blob store <data>/part1/blobs.
  python make_patch.py info OUT.t2pt

Make runs on a PC with RAM (one pack file in memory at a time, ~2.2 GB, plus numpy) - never on the 1 GB server.

Patch format (little endian), version 1:
  0   'T2PT'  u32 version
  8   u64 dst_size   u64 src_size
  24  32 B sha256 of the source file   56  32 B sha256 of the target file
  88  src view: u64 file_off, u64 len, u64 stream_pos      112 dst view: same
  136 u32 op count, u32 0
  144 ops, in target order, covering the target exactly once:
        u32 kind (0 RAW, 1 COPY, 2 RAWZ), u32 0, u64 len, then RAW: len bytes / COPY: u64 source file offset /
        RAWZ: u64 zlen, zlen bytes of zlib data that inflate to the len RAW bytes (pieces of at most 4 MiB)
A view = the file bytes that are part of the encrypted stream: byte x (file_off <= x < file_off + len) is stream byte
stream_pos + (x - file_off). The keystream XORs whole qwords only: bytes past file_off + (len // 8) * 8 are not XOR'd (the stream's
tail). stream_pos is a multiple of 8.
"""
import argparse
import hashlib
import json
import mmap
import os
import shutil
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))

MAGIC = b"T2PT"
VERSION = 1
HDR = struct.Struct("<4sIQQ32s32sQQQQQQII")
OP = struct.Struct("<IIQ")
RAW, COPY, RAWZ = 0, 1, 2
ZPIECE = 4 << 20
DATA_HDR = 0x10000        # t2r-data.bin: 64 KiB header, payload, 16-byte trailer (tools/features/pack_file.py)


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(8 << 20), b""):
            h.update(b)
    return h.digest()


def data_view(path):
    """(file_off, len, stream_pos) of a t2r-data.bin"""
    with open(path, "rb") as f:
        h = f.read(16)
        size = os.fstat(f.fileno()).st_size
    if h[:4] != b"T2RD":
        raise SystemExit("%s: not a t2r-data.bin" % path)
    size1 = struct.unpack_from("<Q", h, 8)[0]
    if DATA_HDR + size1 + 16 != size:
        raise SystemExit("%s: size1 %d does not fit the file size %d" % (path, size1, size))
    return DATA_HDR, size1, 0


def part1_view(data_path, part1_path):
    """(file_off, len, stream_pos) of a part1.dat: the stream's tail after its data file's payload, then a 16-byte build id"""
    _, size1, _ = data_view(data_path)
    n = os.path.getsize(part1_path) - 16
    if n <= 0 or size1 % 8:
        raise SystemExit("%s: no side stream" % part1_path)
    return 0, n, size1


def decrypt_view(buf, view):
    """XOR the view's whole qwords in buf (a bytearray of the whole file) in place"""
    import numpy as np
    import t2r_pack as tp
    off, n, pos = view
    assert pos % 8 == 0
    nq = n // 8
    a = np.frombuffer(buf, dtype="<u8", count=nq, offset=off) if off % 8 == 0 else None
    if a is None:
        raise SystemExit("view not 8-aligned in the file")
    step = 1 << 20
    for i in range(0, nq, step):
        a[i:i + step] = tp._keystream_chunk(a[i:i + step], pos // 8 + i)
    del a


def dsar_dir(data_path):
    """the DSAR entries [(stream off, csize)] from the start of a data file's (decrypted) payload"""
    off, n, pos = data_view(data_path)
    with open(data_path, "rb") as f:
        f.seek(off)
        head = bytearray(f.read(1 << 16))
    decrypt_view(head, (0, len(head), pos))
    magic, _ver, cnt, data_start, _tot = struct.unpack_from("<4sIIIQ", head, 0)
    if magic != b"DSAR":
        raise SystemExit("%s: payload is not a DSAR" % data_path)
    need = 0x28 + cnt * 0x20
    if need > len(head):
        with open(data_path, "rb") as f:
            f.seek(off)
            head = bytearray(f.read((need + 7) // 8 * 8))
        decrypt_view(head, (0, len(head), pos))
    ents = []
    for i in range(cnt):
        eo, _usz, csz = struct.unpack_from("<QII", head, 0x28 + i * 0x20)
        ents.append((eo, csz))
    return ents, data_start


def chunk_hashes(path, view, ents):
    """[(file offset, csize, key)] for the entries that lie in this file's view; key = (csize, blake2b of the decrypted chunk)"""
    off, n, pos = view
    buf = bytearray(os.path.getsize(path))
    with open(path, "rb") as f:
        f.readinto(buf)
    decrypt_view(buf, view)
    out = []
    mv = memoryview(buf)
    for eo, csz in ents:
        if csz and pos <= eo and eo + csz <= pos + n:
            fo = off + (eo - pos)
            out.append((fo, csz, (csz, hashlib.blake2b(mv[fo:fo + csz], digest_size=16).digest())))
    del mv
    return out


def plan(old_path, old_view, old_ents, new_path, new_view, new_ents):
    """ops [[RAW, file_off, len] | [COPY, src_off, len]] for the new file"""
    src = {}
    for fo, _csz, k in chunk_hashes(old_path, old_view, old_ents):
        src.setdefault(k, fo)
    size = os.path.getsize(new_path)
    copies = sorted((fo, csz, src[k]) for fo, csz, k in chunk_hashes(new_path, new_view, new_ents) if k in src)
    ops = []
    cur = 0
    for fo, csz, so in copies:
        if fo < cur:
            continue
        if fo > cur:
            ops.append([RAW, cur, fo - cur])
        if ops and ops[-1][0] == COPY and ops[-1][1] + ops[-1][2] == so:
            ops[-1][2] += csz
        else:
            ops.append([COPY, so, csz])
        cur = fo + csz
    if cur < size:
        ops.append([RAW, cur, size - cur])
    return ops


# ------------------------------------------------------------------ plain files
GEAR_SEED = 0x54325054
CDC_MASK = ((1 << 13) - 1) << 51          # top 13 bits: ~8 KiB between cuts
CDC_MIN, CDC_MAX = 2048, 65536


def cdc_chunks(b):
    """content-defined chunks of the uint8 array b: [(off, len)]. Gear hash h_i = sum_k G[b[i-k]] << k (64 bytes back); a cut after i
    where the top 13 bits of h_i are 0, at least CDC_MIN and at most CDC_MAX apart"""
    import numpy as np
    G = np.random.default_rng(GEAR_SEED).integers(0, 2 ** 63, size=256, dtype=np.uint64) * np.uint64(2) + np.uint64(1)
    seg = 8 << 20
    cands = []
    for a in range(0, len(b), seg):
        lo = max(0, a - 63)
        g = G[b[lo:a + seg]]
        h = g.copy()
        for k in range(1, 64):
            h[k:] += g[:-k] << np.uint64(k)
        hit = np.nonzero((h & np.uint64(CDC_MASK)) == 0)[0] + lo
        cands.append(hit[hit >= a])
    cand = np.concatenate(cands) if cands else np.zeros(0, dtype=np.int64)
    out = []
    start = 0
    n = len(b)
    while start < n:
        lo, hi = start + CDC_MIN, start + CDC_MAX
        j = int(np.searchsorted(cand, lo - 1, side="left"))   # first candidate i with i + 1 >= lo
        end = int(cand[j]) + 1 if j < len(cand) and cand[j] + 1 <= hi else hi
        end = min(end, n)
        out.append((start, end - start))
        start = end
    return out


def plan_file(old_path, new_path):
    """ops for a plain file: chunk matches by content, then every copy grown into the literal on both sides while the bytes agree"""
    import numpy as np
    old = np.fromfile(old_path, dtype=np.uint8)
    new = np.fromfile(new_path, dtype=np.uint8)
    ob, nb = old.tobytes(), new.tobytes()
    src = {}
    for off, n in cdc_chunks(old):
        src.setdefault(hashlib.blake2b(ob[off:off + n], digest_size=16).digest(), off)
    ops = []                                                   # [kind, a, n]: RAW a = new offset, COPY a = old offset
    for off, n in cdc_chunks(new):
        so = src.get(hashlib.blake2b(nb[off:off + n], digest_size=16).digest())
        if so is None:
            if ops and ops[-1][0] == RAW:
                ops[-1][2] += n
            else:
                ops.append([RAW, off, n])
        elif ops and ops[-1][0] == COPY and ops[-1][1] + ops[-1][2] == so:
            ops[-1][2] += n
        else:
            ops.append([COPY, so, n])

    def agree(o, d, n, back=False):
        """how many bytes old[o..] and new[d..] agree (back: old[..o) and new[..d) from the end), at most n"""
        if back:
            n = min(n, o, d)
            if n <= 0:
                return 0
            a, b = old[o - n:o][::-1], new[d - n:d][::-1]
        else:
            n = min(n, len(old) - o, len(new) - d)
            if n <= 0:
                return 0
            a, b = old[o:o + n], new[d:d + n]
        ne = np.nonzero(a != b)[0]
        return int(ne[0]) if len(ne) else n

    pos = []
    p = 0
    for op in ops:
        pos.append(p)
        p += op[2]
    for i, op in enumerate(ops):
        if op[0] != COPY:
            continue
        if i + 1 < len(ops) and ops[i + 1][0] == RAW:          # forward
            r = ops[i + 1]
            k = agree(op[1] + op[2], pos[i] + op[2], r[2])
            op[2] += k
            r[1] += k
            r[2] -= k
            pos[i + 1] += k
        if i > 0 and ops[i - 1][0] == RAW and ops[i - 1][2] > 0:   # backward
            r = ops[i - 1]
            k = agree(op[1], pos[i], r[2], back=True)
            op[1] -= k
            op[2] += k
            r[2] -= k
            pos[i] -= k
    merged = []
    for op in ops:
        if op[2] <= 0:
            continue
        if merged and merged[-1][0] == op[0] and merged[-1][1] + merged[-1][2] == op[1]:
            merged[-1][2] += op[2]
        else:
            merged.append(list(op))
    return merged


def write_patch(out, old_path, old_view, new_path, new_view, ops):
    src_sha, dst_sha = sha256_file(old_path), sha256_file(new_path)
    dsize = os.path.getsize(new_path)
    tmp = out + ".tmp"
    raw = 0
    with open(tmp, "wb") as o, open(new_path, "rb") as nf:
        o.write(HDR.pack(MAGIC, VERSION, dsize, os.path.getsize(old_path), src_sha, dst_sha, *old_view, *new_view, len(ops), 0))
        nops = 0
        for kind, a, n in ops:
            if kind == COPY:
                o.write(OP.pack(kind, 0, n))
                o.write(struct.pack("<Q", a))
                nops += 1
                continue
            nf.seek(a)
            left = n
            while left:                                     # literal pieces: zlib when it saves 10 % (code), as they are when not (LZ4 data)
                b = nf.read(min(left, ZPIECE))
                z = zlib.compress(b, 9)
                if len(z) < len(b) * 0.9:
                    o.write(OP.pack(RAWZ, 0, len(b)) + struct.pack("<Q", len(z)))
                    o.write(z)
                    raw += len(z)
                else:
                    o.write(OP.pack(RAW, 0, len(b)))
                    o.write(b)
                    raw += len(b)
                nops += 1
                left -= len(b)
        o.seek(HDR.size - 8)                                # the op count (literals were split into pieces)
        o.write(struct.pack("<I", nops))
    os.replace(tmp, out)
    return src_sha.hex(), dst_sha.hex(), raw


def check_ops(ops, dsize, ssize):
    pos = 0
    for kind, a, n in ops:
        assert n > 0
        if kind == COPY:
            assert 0 <= a and a + n <= ssize
        else:
            assert a == pos
        pos += n
    assert pos == dsize, (pos, dsize)


def read_header(path):
    with open(path, "rb") as f:
        h = HDR.unpack(f.read(HDR.size))
    if h[0] != MAGIC or h[1] != VERSION:
        raise SystemExit("%s: not a T2PT v1 patch" % path)
    return {"dst_size": h[2], "src_size": h[3], "from": h[4].hex(), "to": h[5].hex(), "src_view": h[6:9], "dst_view": h[9:12], "ops": h[12]}


def make(kind, args, out):
    if kind == "file":
        old_path, new_path = args
        ops = plan_file(old_path, new_path)
        check_ops(ops, os.path.getsize(new_path), os.path.getsize(old_path))
        return finish(out, old_path, (0, 0, 0), new_path, (0, 0, 0), ops)
    if kind == "data":
        old_data, new_data = args
        old_path, new_path = old_data, new_data
        old_view, new_view = data_view(old_data), data_view(new_data)
    else:
        old_data, old_path, new_data, new_path = args
        old_view, new_view = part1_view(old_data, old_path), part1_view(new_data, new_path)
    old_ents, _ = dsar_dir(old_data)
    new_ents, _ = dsar_dir(new_data)
    ops = plan(old_path, old_view, old_ents, new_path, new_view, new_ents)
    check_ops(ops, os.path.getsize(new_path), os.path.getsize(old_path))
    return finish(out, old_path, old_view, new_path, new_view, ops)


def finish(out, old_path, old_view, new_path, new_view, ops):
    frm, to, raw = write_patch(out, old_path, old_view, new_path, new_view, ops)
    cp = sum(n for k, _a, n in ops if k == COPY)
    psha = hashlib.sha256(open(out, "rb").read()).hexdigest()
    info = {"from": frm, "to": to, "sha256": psha, "size": os.path.getsize(out), "copy_ops": sum(1 for k, _a, _n in ops if k == COPY), "copied": cp, "literal": raw}
    print(json.dumps(info))
    return info


def install(data, manifest, patch, part1=False):
    h = read_header(patch)
    psha = sha256_file(patch).hex()
    size = os.path.getsize(patch)
    with open(manifest, encoding="utf-8") as f:
        m = json.load(f)
    hits = [f for f in m["files"] if f.get("sha256") == h["to"]]
    if not hits:
        raise SystemExit("no file in %s has the patch's target sha %s" % (manifest, h["to"]))
    blobs = os.path.join(data, "part1", "blobs") if part1 else os.path.join(data, "blobs")
    os.makedirs(blobs, exist_ok=True)
    dst = os.path.join(blobs, psha)
    if not (os.path.exists(dst) and os.path.getsize(dst) == size):
        shutil.copyfile(patch, dst + ".tmp")
        os.replace(dst + ".tmp", dst)
    for f in hits:
        if f.get("size") != h["dst_size"]:
            raise SystemExit("%s: manifest size %s != patch target size %d" % (f.get("path"), f.get("size"), h["dst_size"]))
        ps = [p for p in f.get("patches", []) if p.get("from") != h["from"]]
        ps.append({"from": h["from"], "sha256": psha, "size": size})
        f["patches"] = ps
    tmp = manifest + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(m, f, indent=1)
    os.replace(tmp, manifest)
    print("installed patch %s (%d bytes) for %s, from %s" % (psha[:16], size, ", ".join(f["path"] for f in hits), h["from"][:16]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["file", "data", "part1", "install", "info"])
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--data")
    ap.add_argument("--manifest")
    ap.add_argument("--part1", action="store_true")
    a = ap.parse_args()
    if a.cmd == "file":
        if len(a.paths) != 3:
            ap.error("file OLD NEW OUT")
        make("file", a.paths[:2], a.paths[2])
    elif a.cmd == "data":
        if len(a.paths) != 3:
            ap.error("data OLD_DATA NEW_DATA OUT")
        make("data", a.paths[:2], a.paths[2])
    elif a.cmd == "part1":
        if len(a.paths) != 5:
            ap.error("part1 OLD_DATA OLD_PART1 NEW_DATA NEW_PART1 OUT")
        make("part1", a.paths[:4], a.paths[4])
    elif a.cmd == "install":
        if not a.data or not a.manifest or len(a.paths) != 1:
            ap.error("install --data DIR --manifest M PATCH")
        install(a.data, a.manifest, a.paths[0], a.part1)
    else:
        print(json.dumps(read_header(a.paths[0])))


if __name__ == "__main__":
    main()
