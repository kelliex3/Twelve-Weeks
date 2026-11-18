"""Unpack / repack the mod content blob inside the T2R client winmm.dll.

Layer stack (outer -> inner), all reverse-engineered from winmm.dll:
  T2ME   resource RCDATA 1001: 'T2ME' u32 ver=4, u64 payload size, u64 FNV-1a-64 of the
         ENCRYPTED payload (qword-wise, then tail bytes; checked by loader 0x180001180),
         payload XOR'd with a per-qword SplitMix64 keystream (no key).
  DSAR   Naughty Dog archive (same as the game's own build/pc/main/*.psarc): header 0x28,
         entries 0x20 {u64 off, u32 usize, u32 csize, u64 tag, u64 cumulative uend}; the
         last entry's final 8 bytes overlap the data start (dataStart = dirEnd - 8).
         Each entry = one LZ4 block.
  PSARC  Sony archive (big endian, zlib 64 KiB blocks, TOC sorted by MD5(path),
         entry 0 = manifest of '\\n'-separated paths).

  python t2r_pack.py unpack <winmm.dll> <out_dir>
  python t2r_pack.py pack   <winmm.dll> <in_dir> <out.dll>
  python t2r_pack.py verify <winmm.dll>             # decode + full self-check
Run with tools/win/.venv (needs numpy, lz4, pefile).
"""
import concurrent.futures
import ctypes
import hashlib
import os
import struct
import subprocess
import sys
import time
import zlib

import lz4.block
import numpy as np
import pefile

M64 = (1 << 64) - 1
DSAR_TAG = 0x5555555555555503          # lz4 entries
DSAR_TAG_STORED = 0x5555555555555403   # the stock archives' tag of their stored (csz == usz) chunks
DSAR_CHUNK = 0x40000          # 256 KiB uncompressed per DSAR entry
PSARC_BLOCK = 0x10000


# ------------------------------------------------------------------ T2ME layer
def _keystream_chunk(c, i0):
    i = np.arange(i0, i0 + len(c), dtype=np.uint64)
    with np.errstate(over="ignore"):
        x = i * np.uint64(0x9E3779B97F4A7C15) + np.uint64(0xED9AE8F47523C0FB)
        x ^= np.uint64(0xECBDD9B9C0033EFF)
        x = (x ^ (x >> np.uint64(30))) * np.uint64(0xBF58476D1CE4E5B9)
        x = (x ^ (x >> np.uint64(27))) * np.uint64(0x94D049BB133111EB)
        x ^= x >> np.uint64(31)
    return c ^ x


def keystream_xor(buf):
    nq = len(buf) // 8
    c = np.frombuffer(buf[:nq * 8], dtype="<u8")
    step = 1 << 22                                     # (qwords per task; numpy drops the GIL in these ufuncs)
    with concurrent.futures.ThreadPoolExecutor(max_workers=_workers()) as ex:
        parts = list(ex.map(lambda a: _keystream_chunk(c[a:a + step], a), range(0, nq, step)))
    return b"".join(x.tobytes() for x in parts) + bytes(buf[nq * 8:])


def fnv_payload_py(buf):
    """FNV-1a-64 over 8-byte little-endian words, then the remaining bytes (pure python: ~7-15 s per 450 MB)."""
    nq = len(buf) // 8
    h = 0xCBF29CE484222325
    for v in np.frombuffer(buf[:nq * 8], dtype="<u8").tolist():
        h = ((h ^ v) * 0x100000001B3) & M64
    for b in buf[nq * 8:]:
        h = ((h ^ b) * 0x100000001B3) & M64
    return h


_FNV_C = r"""
#include <stdint.h>
#include <stddef.h>
uint64_t fnv_payload(const uint8_t *p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ULL;
    size_t nq = n / 8, i;
    const uint64_t *q = (const uint64_t *)p;
    for (i = 0; i < nq; i++) h = (h ^ q[i]) * 0x100000001B3ULL;
    for (i = nq * 8; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ULL;
    return h;
}
"""
_fnv_lib = None


def _fnv_native():
    """The same hash as a tiny C DLL (gcc from the repo's msys64 or PATH), compiled once into tools/local/build-cache;
    None = no compiler (the pure python version gives the same value)."""
    global _fnv_lib
    if _fnv_lib is not None:
        return _fnv_lib or None
    _fnv_lib = False
    if os.environ.get("T2R_NO_NATIVE_FNV"):
        return None
    try:
        here = os.path.dirname(os.path.abspath(__file__))
        d = os.path.join(here, "local", "build-cache")
        os.makedirs(d, exist_ok=True)
        tag = hashlib.sha1(_FNV_C.encode()).hexdigest()[:10]
        dll = os.path.join(d, f"fnv64-{tag}.dll")
        if not os.path.exists(dll):
            gcc = os.path.join(here, "win", "ext", "msys64", "ucrt64", "bin", "gcc.exe")
            env = dict(os.environ, PATH=os.path.dirname(gcc) + os.pathsep + os.environ.get("PATH", ""))
            if not os.path.exists(gcc):
                gcc = "gcc"
            src = os.path.join(d, f"fnv64-{tag}.c")
            with open(src, "w") as f:
                f.write(_FNV_C)
            tmp = dll + f".{os.getpid()}.tmp"
            subprocess.run([gcc, "-O2", "-shared", "-o", tmp, src], check=True, env=env, capture_output=True)
            os.replace(tmp, dll)
        lib = ctypes.CDLL(dll)
        lib.fnv_payload.restype = ctypes.c_uint64
        lib.fnv_payload.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        t = bytes(range(256)) * 3 + b"xyz"           # self-test against the python version (word loop + tail)
        if lib.fnv_payload(t, len(t)) != fnv_payload_py(t):
            return None
        _fnv_lib = lib
        return lib
    except Exception:
        return None


def fnv_payload(buf):
    lib = _fnv_native()
    if lib is not None:
        return lib.fnv_payload(buf if isinstance(buf, bytes) else bytes(buf), len(buf))
    return fnv_payload_py(buf)


def find_blob(dll_bytes):
    pe = pefile.PE(data=dll_bytes, fast_load=True)
    pe.parse_data_directories([pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"]])
    for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        if t.id != 10:                                   # RT_RCDATA
            continue
        for n in t.directory.entries:
            if n.id == 1001:
                ent = n.directory.entries[0].data.struct
                off = pe.get_offset_from_rva(ent.OffsetToData)
                return pe, ent, off
    raise SystemExit("RCDATA 1001 (T2ME blob) not found")


def read_t2me(dll_bytes):
    _, ent, off = find_blob(dll_bytes)
    magic, ver, size, h = struct.unpack_from("<4sIQQ", dll_bytes, off)
    assert magic == b"T2ME" and ver == 4, (magic, ver)
    enc = dll_bytes[off + 0x18: off + 0x18 + size]
    return enc, h


def build_t2me(dec_payload):
    enc = keystream_xor(dec_payload)
    return struct.pack("<4sIQQ", b"T2ME", 4, len(enc), fnv_payload(enc)) + enc


# ------------------------------------------------------------------ DSAR layer
def dsar_read(buf):
    magic, ver, n, data_start, total = struct.unpack_from("<4sIIIQ", buf, 0)
    assert magic == b"DSAR", magic
    mv = memoryview(buf)

    def one(i):
        off, usz, csz = struct.unpack_from("<QII", buf, 0x28 + i * 0x20)
        if csz == 0:
            return b"\0" * usz                           # stock archives: all-zero chunk
        if csz == usz:
            return bytes(mv[off:off + csz])              # stored (stock game archives have some)
        return lz4.block.decompress(mv[off:off + csz], uncompressed_size=usz)

    with concurrent.futures.ThreadPoolExecutor(max_workers=_workers()) as ex:    # (python-lz4 drops the GIL)
        out = b"".join(ex.map(one, range(n), chunksize=16))
    assert len(out) == total, (len(out), total)
    return out


def _workers():
    return max(1, int(os.environ.get("T2R_PACK_THREADS") or (os.cpu_count() or 4)))


def lz4_literal_block(raw):
    """One lz4 sequence of literals only (token 0xF0 + length bytes + the bytes)."""
    n = len(raw)
    if n < 15:
        return bytes([n << 4]) + raw
    r = n - 15
    return bytes([0xF0]) + bytes([0xFF]) * (r // 255) + bytes([r % 255]) + raw


LZ4_LEVEL = int(os.environ.get("T2R_LZ4_LEVEL") or (4 if os.environ.get("T2R_FAST_PACK") == "1" else 12))   # T2R_FAST_PACK=1: A/B test builds only     # 12 = the shipped setting (release); a dev build may set less: only the size differs
_CACHE_DAYS = 14


def _chunk_cache_dir():
    d = os.environ.get("T2R_PACK_CACHE")
    if d == "0":
        return None
    if not d:
        d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "local", "build-cache", "lz4")
    try:
        os.makedirs(d, exist_ok=True)
        return d
    except OSError:
        return None


def _lz4_chunk(raw, cache_dir):
    """one DSAR entry's lz4 block; deterministic in (raw, level), so the on-disk cache by content hash gives the same bytes"""
    path = None
    if cache_dir:
        key = hashlib.blake2b(raw, digest_size=20).hexdigest()
        path = os.path.join(cache_dir, key[:2], f"{key}-{len(raw)}-L{LZ4_LEVEL}")
        try:
            with open(path, "rb") as f:
                c = f.read()
            os.utime(path, None)
            return c
        except OSError:
            pass
    c = lz4.block.compress(raw, mode="high_compression", compression=LZ4_LEVEL, store_size=False)
    if len(c) == len(raw):
        c = lz4_literal_block(raw)
    if path:
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            tmp = f"{path}.{os.getpid()}.{id(raw)}.tmp"
            with open(tmp, "wb") as f:
                f.write(c)
            os.replace(tmp, path)
        except OSError:
            pass
    return c


def _prune_cache(cache_dir):
    """drop cache entries nobody used for a while (every hit refreshes the mtime)"""
    old = time.time() - _CACHE_DAYS * 86400
    try:
        for sub in os.scandir(cache_dir):
            if sub.is_dir():
                for e in os.scandir(sub.path):
                    if e.stat().st_mtime < old:
                        os.remove(e.path)
    except OSError:
        pass


def dsar_write(data, cuts, align=None):
    """cuts: sorted offsets where a new entry must start (entries also split at 256 KiB).
    align = (data offset, modulus, remainder): the chunk that starts at that data offset is placed so that its position in the DSAR is
    == remainder (mod modulus), zero padding before it (T2R_P1_SIDE_VIRT: the side-file region must start on a 64 KiB view boundary)."""
    bounds = sorted(set([0] + [c for c in cuts if 0 < c < len(data)]))
    spans = []
    for a, b in zip(bounds, bounds[1:] + [len(data)]):
        while a < b:
            spans.append((a, min(b, a + DSAR_CHUNK)))
            a = spans[-1][1]
    # Every chunk is an lz4 block - stored (csz == usz) chunks broke every mod PNG (custom icons, map images) under either entry
    # tag, the game does not read them. An lz4 block of EXACTLY usz bytes would read back as stored, so that chunk is encoded as a
    # literal-only lz4 block instead (always usz + 1 + (usz - 15) // 255 + 1 bytes, a valid block every decoder accepts).
    n = len(spans)
    data_start = 0x28 + n * 0x20 - 8
    blocks, entries, pos = [], [], data_start
    cache_dir = _chunk_cache_dir()
    mv = memoryview(data)
    # the chunks are independent: compressed on all cores (python-lz4 drops the GIL) and cached by content hash
    with concurrent.futures.ThreadPoolExecutor(max_workers=_workers()) as ex:
        comp = list(ex.map(lambda ab: _lz4_chunk(bytes(mv[ab[0]:ab[1]]), cache_dir), spans, chunksize=4))
    if cache_dir:
        _prune_cache(cache_dir)
    for (a, b), c in zip(spans, comp):
        if align and a == align[0] and pos % align[1] != align[2]:
            padn = (align[2] - pos) % align[1]
            blocks.append(b"\0" * padn)
            pos += padn
        tag = DSAR_TAG
        entries.append((pos, b - a, len(c), b, tag))
        blocks.append(c)
        pos += len(c)
        pad = (-pos) & 0xF
        if pad and (a, b) != spans[-1]:
            blocks.append(b"\0" * pad)
            pos += pad
    head = struct.pack("<4sIIIQ", b"DSAR", 0x00010003, n, data_start, len(data)) + b"PADDING*" + b"\0" * 8
    dir_ = bytearray()
    for off, usz, csz, uend, tag in entries:
        dir_ += struct.pack("<QIIQQ", off, usz, csz, tag, uend)
    dir_ = dir_[:-8]                                     # last 8 bytes overlap the data start
    return head + bytes(dir_) + b"".join(blocks)


# ------------------------------------------------------------------ PSARC layer
def psarc_read(psar):
    magic, vmaj, vmin, comp, toc_len, esz, n, bsize, flags = struct.unpack_from(">4sHH4sIIIII", psar, 0)
    assert magic == b"PSAR" and comp == b"zlib" and esz == 30, (magic, comp, esz)
    toc = []
    for i in range(n):
        e = psar[32 + i * 30: 32 + (i + 1) * 30]
        toc.append((e[:16], int.from_bytes(e[16:20], "big"),
                    int.from_bytes(e[20:25], "big"), int.from_bytes(e[25:30], "big")))
    bt_off = 32 + n * 30
    nblk = (toc_len - bt_off) // 2
    bsizes = struct.unpack_from(f">{nblk}H", psar, bt_off)

    def file_bytes(bi, usz, off):
        # fast path: a file whose blocks are all stored raw (what psarc_write makes) is one contiguous slice
        raw, o2, got = True, off, 0
        while got < usz:
            cs = bsizes[bi + got // bsize] or bsize
            want = min(bsize, usz - got)
            if cs != want:
                raw = False
                break
            o2 += cs
            got += want
        if raw:
            return bytes(psar[off:o2])
        out = bytearray()
        while len(out) < usz:
            cs = bsizes[bi] or bsize
            blk = psar[off:off + cs]
            want = min(bsize, usz - len(out))
            if cs == want:
                out += blk                               # stored raw
            else:
                out += zlib.decompress(blk)
            off += cs
            bi += 1
        return bytes(out)

    manifest = file_bytes(*toc[0][1:]).decode()
    paths = manifest.split("\0")
    trail = paths[-1] == ""                              # remember a trailing NUL
    if trail:
        paths = paths[:-1]
    by_md5 = {hashlib.md5(p.encode()).digest(): p for p in paths}
    files = {}
    order = []
    for md5, bi, usz, off in toc[1:]:
        p = by_md5[md5]
        files[p] = file_bytes(bi, usz, off)
        order.append((off, p))
    data_order = [p for _, p in sorted(order)]           # physical order in the archive
    return {"flags": flags, "paths": paths, "data_order": data_order, "trail": trail}, files


def psarc_write(meta, files):
    paths = meta["paths"]
    manifest = "\0".join(paths).encode() + (b"\0" if meta.get("trail") else b"")
    # TOC: manifest first, then entries sorted by MD5(path) (as the original)
    ents = sorted(paths, key=lambda p: hashlib.md5(p.encode()).digest())
    data_order = [p for p in meta["data_order"] if p in files] + [p for p in paths if p not in meta["data_order"]]
    blobs, bsizes = {}, []

    def blocks_of(data):
        # Store blocks RAW like the original (4329 full + 135 partial raw blocks, 1 zlib):
        # the mod's virtual-file reader expects uncompressed file data, and compression
        # happens one layer up (LZ4 per DSAR entry). zlib blocks -> "UNKNOWN STRING!!!".
        res = [data[a:a + PSARC_BLOCK] for a in range(0, len(data), PSARC_BLOCK)]
        return res or [b""]

    n = len(paths) + 1
    toc_len_guess = 32 + n * 30
    order = [None] + data_order                          # None = manifest
    pieces, layout, bi = [], {}, 0
    for p in order:
        data = manifest if p is None else files[p]
        # (MemoryError next to a running game: the block slices + their join copied every file twice; the sizes come
        # from arithmetic now and the file bytes go in as they are - the same bytes as joining its consecutive PSARC_BLOCK slices)
        nfull, rest = divmod(len(data), PSARC_BLOCK)
        sizes = [PSARC_BLOCK] * nfull + ([rest] if rest or not nfull else [])
        layout[p] = (bi, len(data), len(data))
        for x in sizes:
            bsizes.append(0 if x == PSARC_BLOCK else x)
        bi += len(sizes)
        pieces.append(data)
    toc_len = toc_len_guess + 2 * len(bsizes)
    off = toc_len
    offsets = {}
    for p, piece in zip(order, pieces):
        offsets[p] = off
        off += len(piece)

    def ent(md5, p):
        bi_, usz, _ = layout[p]
        return md5 + bi_.to_bytes(4, "big") + usz.to_bytes(5, "big") + offsets[p].to_bytes(5, "big")

    toc = ent(b"\0" * 16, None) + b"".join(ent(hashlib.md5(p.encode()).digest(), p) for p in ents)
    head = struct.pack(">4sHH4sIIIII", b"PSAR", 1, 4, b"zlib", toc_len, 30, n, PSARC_BLOCK, meta["flags"])
    hdr = head + toc + struct.pack(f">{len(bsizes)}H", *bsizes)
    out = bytearray(len(hdr) + sum(len(x) for x in pieces))     # one allocation of the final size ('out += piece' regrowth hit MemoryError)
    out[:len(hdr)] = hdr
    pos = len(hdr)
    for piece in pieces:
        out[pos:pos + len(piece)] = piece
        pos += len(piece)
    out = bytes(out) if os.environ.get("T2R_PSARC_BYTES") else out   # (bytearray: no second full-size copy)
    cuts = [toc_len] + [offsets[p] for p in order]          # DSAR entries start at file starts
    return out, cuts


def split_payload(meta, files, side_paths, delta=0, modulus=0x10000):
    """T2R_P1_SIDE_VIRT (re/part1/side-files.md): the same archive as pack_bytes builds, but the data of `side_paths` is laid out LAST and cut off.
    -> (phys, side, info): phys = the T2ME payload (encrypted DSAR up to the first side chunk), side = the encrypted remainder (the side file's
    bytes). Playing them back to back (phys, then side starting at payload position T) is byte-identical to the unsplit encrypted DSAR, so every
    offset in the directory, the keystream index and the PSARC TOC stay valid; T satisfies (delta + T) % modulus == 0, i.e. if the payload
    starts `delta` bytes into a 64 KiB-aligned file view, the side file mapped right behind the view starts on a view boundary."""
    sp = set(p for p in side_paths if p in files)
    meta2 = dict(meta, data_order=[p for p in meta["data_order"] if p not in sp] + sorted(sp))
    psar, cuts = psarc_write(meta2, files)
    first = None                                         # PSARC offset of the first side file's data (TOC entry: md5, block idx, usize, offset)
    if sp:
        n = struct.unpack_from(">I", psar, 20)[0]           # TOC entry COUNT (offset 16 is the entry SIZE, 30: only 30 entries were searched)
        want = {hashlib.md5(p.encode()).digest(): p for p in sp}
        offs = []
        for i in range(n):
            e = psar[32 + i * 30: 32 + (i + 1) * 30]
            if bytes(e[:16]) in want:
                offs.append(int.from_bytes(e[25:30], "big"))
        assert offs, "split_payload: none of the %d side files found in the PSARC TOC" % len(sp)
        first = min(offs)
    align = (first, modulus, (-delta) % modulus) if first is not None else None
    dsar = dsar_write(psar, cuts, align)
    t = len(dsar)
    if first is not None:                                # DSAR position of the chunk that holds PSARC offset `first`
        n_ent = struct.unpack_from("<I", dsar, 8)[0]
        tot = 0
        for i in range(n_ent):
            off, usz, csz = struct.unpack_from("<QII", dsar, 0x28 + i * 0x20)
            if tot == first:
                t = off
                break
            tot += usz
        else:
            raise AssertionError("first side chunk not found")
    enc = keystream_xor(dsar)
    return enc[:t], enc[t:], {"cut": t, "dsar": dsar, "size": len(dsar), "first": first}


# ------------------------------------------------------------------ DLL layer
def write_dll(dll_bytes, t2me):
    """Put a new T2ME blob into the DLL. Fits in place when not larger; otherwise the
    blob moves into an appended '.t2rc' section and the resource entry is re-pointed
    (the loader accepts the resource anywhere inside a mapped section)."""
    pe, ent, off = find_blob(dll_bytes)
    out = bytearray(dll_bytes)
    print(f"mod blob {len(t2me)} bytes, resource {ent.Size} ({ent.Size - len(t2me)} free)")
    if len(t2me) <= ent.Size:
        out[off:off + len(t2me)] = t2me
        out[off + len(t2me):off + ent.Size] = b"\0" * (ent.Size - len(t2me))
        return bytes(out)
    fa = pe.OPTIONAL_HEADER.FileAlignment
    sa = pe.OPTIONAL_HEADER.SectionAlignment
    last = pe.sections[-1]
    raw_ptr = (len(out) + fa - 1) // fa * fa
    rva = (last.VirtualAddress + last.Misc_VirtualSize + sa - 1) // sa * sa
    raw_size = (len(t2me) + fa - 1) // fa * fa
    hdr_off = pe.DOS_HEADER.e_lfanew + 4 + 20 + pe.FILE_HEADER.SizeOfOptionalHeader
    sec_off = hdr_off + 40 * pe.FILE_HEADER.NumberOfSections
    if sec_off + 40 > pe.OPTIONAL_HEADER.SizeOfHeaders:
        raise SystemExit("no room for another section header")
    out[sec_off:sec_off + 40] = struct.pack("<8sIIIIIIHHI", b".t2rc", len(t2me), rva, raw_size,
                                            raw_ptr, 0, 0, 0, 0, 0x40000040)   # INITIALIZED_DATA|READ
    struct.pack_into("<H", out, pe.DOS_HEADER.e_lfanew + 4 + 2, pe.FILE_HEADER.NumberOfSections + 1)
    soi_off = pe.OPTIONAL_HEADER.get_file_offset() + 56
    struct.pack_into("<I", out, soi_off, (rva + len(t2me) + sa - 1) // sa * sa)
    # the old resource now holds a dead copy of the unpatched DLL blob (~160 MB, incompressible):
    # zero it so archives of the DLL compress it away (the client .rar had doubled)
    out[off:off + ent.Size] = b"\0" * ent.Size
    out += b"\0" * (raw_ptr - len(out))
    out += t2me + b"\0" * (raw_size - len(t2me))
    ent_off = ent.get_file_offset()
    struct.pack_into("<II", out, ent_off, rva, len(t2me))
    struct.pack_into("<I", out, pe.OPTIONAL_HEADER.get_file_offset() + 64, 0)   # CheckSum: not enforced for DLLs
    return bytes(out)


def payload_file_offset(dll_bytes):
    """FILE offset of the T2ME payload (= what winmm keeps in [0x1801398e8]) in a finished image"""
    _, ent, off = find_blob(dll_bytes)
    return off + 0x18


def pack_split_bytes(dll, in_dir, side_paths, delta):
    """T2R_P1_SIDE virt build: like pack_bytes, but the data of `side_paths` is cut off (split_payload) and returned as the side file.
    -> (image bytes with the cut payload, side file bytes = encrypted tail + 16-byte build id, info {size1, size2, build_id, ...})"""
    meta, files = load_dir(in_dir)
    phys, side, info = split_payload(meta, files, side_paths, delta)
    t2me = struct.pack("<4sIQQ", b"T2ME", 4, len(phys), fnv_payload(phys)) + phys
    out = write_dll(dll if isinstance(dll, (bytes, bytearray)) else open(dll, "rb").read(), t2me)
    assert read_t2me(out)[0] == phys, "T2ME blob in the image differs from the one built"
    if os.environ.get("T2R_FAST_PACK") != "1":
        m2, f2 = psarc_read(dsar_read(info["dsar"]))
        assert f2 == files and m2["paths"] == meta["paths"], "round-trip mismatch"
    bid = hashlib.sha256(phys).digest()[:16]
    print(f"{len(files)} files, blob {len(t2me) / 1e6:.1f} MB + side {len(side) / 1e6:.1f} MB, verified")
    return out, side + bid, {"size1": len(phys), "size2": len(side), "build_id": bid, "cut": info["cut"], "delta": delta}


# ------------------------------------------------------------------ commands
def unpack(dll, out_dir):
    enc, h = read_t2me(open(dll, "rb").read())
    assert fnv_payload(enc) == h, "T2ME hash mismatch"
    meta, files = psarc_read(dsar_read(keystream_xor(enc)))
    for p, data in files.items():
        dst = os.path.join(out_dir, p.lstrip("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, "wb").write(data)
    with open(os.path.join(out_dir, "_t2r_manifest.txt"), "w", newline="\n") as f:
        f.write(f"flags={meta['flags']} trail={int(meta['trail'])}\n")
        f.write("\n".join(meta["paths"]) + "\n")
        f.write("--order--\n" + "\n".join(meta["data_order"]) + "\n")
    print(f"{len(files)} files -> {out_dir}")


def _gapless(p, data):
    """a pak whose VRAM trailer has bytes no VRAM_DESC owns -> the same pak with the trailer re-laid without gaps (pak_graft.compact_trailer).
    TextureMgrPub::ConsumeData asserts (tlou-ii.exe+12e7ee5) when a streamed chunk starts in such bytes; lightmap_fix / probe_fix injections
    append and leave the replaced payload (their undo records need it there), so the stage keeps the gaps and only the packed copy is compacted
    (release 165u: Supermarket load crash on A and B, gaps in pat-jackson-supermarket(-int).pak)."""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "models"))
    import pak_graft
    try:
        t = pak_graft.snow_clone.Pak(data)
        if not pak_graft.trailer_gaps(t):
            return data
    except Exception:
        return data                                   # (not a texture pak layout this parser reads: left as is)
    out = pak_graft.compact_trailer(data)
    left = pak_graft.trailer_gaps(pak_graft.snow_clone.Pak(out))
    if left:
        raise SystemExit("t2r_pack: %s still has texture data gaps after compaction: %s" % (p, left[:2]))
    print("t2r_pack: %s texture trailer compacted (%d -> %d bytes)" % (p, len(data), len(out)))
    return out


STUB_PAYLOAD = 0x10000      # T2R_PACK_FILE: payload bytes kept in the image (tools/features/pack_file.py STUB)


def write_dll_stub(dll_bytes, prefix):
    """T2R_PACK_FILE (tools/features/pack_file.py): a small T2ME blob (header + the payload's first bytes) written IN PLACE of the original
    resource; the resource entry's size becomes the stub size so shrink_rsrc drops the rest from the file. No `.t2rc` section: SizeOfImage
    stays the base image's. The loader checks the stub (magic, version, size, FNV) as before; the cave then points [d0]/[d8] at the file."""
    pe, ent, off = find_blob(dll_bytes)
    t2me = struct.pack("<4sIQQ", b"T2ME", 4, len(prefix), fnv_payload(prefix)) + bytes(prefix)
    assert len(t2me) <= ent.Size and len(t2me) < 0x100000, (len(t2me), ent.Size)
    out = bytearray(dll_bytes)
    out[off:off + ent.Size] = b"\0" * ent.Size
    out[off:off + len(t2me)] = t2me
    struct.pack_into("<I", out, ent.get_file_offset() + 4, len(t2me))
    struct.pack_into("<I", out, pe.OPTIONAL_HEADER.get_file_offset() + 64, 0)   # CheckSum: not enforced for DLLs
    return bytes(out)


def pack_file_bytes(dll, in_dir, side_paths=()):
    """T2R_PACK_FILE build: the whole encrypted payload goes to t2r-data.bin (pack_file.write_data_file), the image keeps a stub.
    side_paths (P1-side builds): their data is laid out last and cut at a 64 KiB boundary (split_payload, delta 0: the payload starts on a
    view boundary in t2r-data.bin) and returned as part1.dat bytes.
    -> (image bytes, payload, side file bytes (encrypted tail + 16-byte build id) or None, info {size1, size2, build_id, fnv})"""
    meta, files = load_dir(in_dir)
    phys, side, info = split_payload(meta, files, side_paths, 0)
    if side_paths:
        assert side and len(phys) % 0x10000 == 0, "pack file: the Part I cut is not on a 64 KiB boundary"
    fnv = fnv_payload(phys)
    out = write_dll_stub(dll if isinstance(dll, (bytes, bytearray)) else open(dll, "rb").read(), phys[:STUB_PAYLOAD])
    enc, _h = read_t2me(out)
    assert enc == phys[:STUB_PAYLOAD], "T2ME stub in the image differs from the one built"
    if os.environ.get("T2R_FAST_PACK") != "1":
        m2, f2 = psarc_read(dsar_read(info["dsar"]), files)
        assert f2 == files and m2["paths"] == meta["paths"], "round-trip mismatch"
    del info["dsar"]
    bid = hashlib.sha256(phys).digest()[:16]
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "features"))
    import pack_file
    sfnv = pack_file.sampled_fnv(phys)
    print(f"{len(files)} files, data file payload {len(phys) / 1e6:.1f} MB" + (f" + side {len(side) / 1e6:.1f} MB" if side else "") + ", verified")
    return out, phys, (side + bid) if side else None, {"size1": len(phys), "size2": len(side), "build_id": bid, "fnv": fnv, "cut": info["cut"], "delta": 0,
                                                 "sfnv": sfnv}


def load_dir(in_dir):
    lines = open(os.path.join(in_dir, "_t2r_manifest.txt")).read().split("\n")
    kv = dict(x.split("=") for x in lines[0].split())
    flags, trail = int(kv["flags"]), bool(int(kv["trail"]))
    k = lines.index("--order--")
    paths = [l for l in lines[1:k] if l]
    order = [l for l in lines[k + 1:] if l]
    def rd(p):
        with open(os.path.join(in_dir, p.lstrip("/")), "rb") as f:
            data = f.read()
        return data
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(8, _workers())) as ex:      # (disk reads overlap; the dict keeps the manifest order)
        files = dict(zip(paths, ex.map(rd, paths)))
    return {"flags": flags, "paths": paths, "data_order": order, "trail": trail}, files


def pack_bytes(dll, in_dir, drop=()):
    """the DLL image (bytes) with the mod directory packed into it (what `pack` writes), without touching the disk for the result.
    drop: paths left out of the archive (T2R_SLIM: files identical to ones the engine mounts from the game's own archives)"""
    meta, files = load_dir(in_dir)
    if drop:
        dset = set(drop)
        meta = dict(meta, paths=[p for p in meta["paths"] if p not in dset], data_order=[p for p in meta["data_order"] if p not in dset])
        files = {p: d for p, d in files.items() if p not in dset}
    psar, cuts = psarc_write(meta, files)
    dsar = dsar_write(psar, cuts)
    t2me = build_t2me(dsar)
    out = write_dll(dll if isinstance(dll, (bytes, bytearray)) else open(dll, "rb").read(), t2me)
    # self-check: the blob inside the image is the one we built, and it decodes back to the files (dsar -> psarc layers)
    assert read_t2me(out)[0] == t2me[0x18:], "T2ME blob in the image differs from the one built"
    if os.environ.get("T2R_FAST_PACK") != "1":       # (A/B test builds skip the 14 s decode-back; the T2ME blob check above stays)
        m2, f2 = psarc_read(dsar_read(dsar))
        assert f2 == files and m2["paths"] == meta["paths"], "round-trip mismatch"
    print(f"{len(files)} files, blob {len(t2me) / 1e6:.1f} MB, verified")
    return out


def pack(dll, in_dir, out_dll):
    out = pack_bytes(dll, in_dir)
    open(out_dll, "wb").write(out)
    print(f"{out_dll}: written")


def verify(dll):
    enc, h = read_t2me(open(dll, "rb").read())
    ok = fnv_payload(enc) == h
    meta, files = psarc_read(dsar_read(keystream_xor(enc)))
    print(f"hash {'OK' if ok else 'BAD'}, {len(files)} files, {sum(map(len, files.values())) / 1e6:.1f} MB")


if __name__ == "__main__":
    cmd, *args = sys.argv[1:]
    {"unpack": unpack, "pack": pack, "verify": verify}[cmd](*args)
