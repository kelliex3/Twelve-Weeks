"""Slim client builds (T2R_SLIM=1, default OFF): do not ship what the player already owns. See re/slim-build.md.

The mod archive embedded in winmm.dll (T2ME blob = XOR-encrypted DSAR of lz4 chunks around a PSARC) carries ~660 MB of
files that are edited copies of files the game ships (game-pak mods: level paks with a few bytes changed, weapon / anim
paks, sound banks). For those the build ships a zstd patch against the game's original (zstd --patch-from, ~50 MB in
all) and zeroes the files' lz4 chunks inside the DLL (zeros zip away). On the player's PC release/client/slim/t2r-slim.exe
reads the originals from the game's own psarc files, applies the patches, recompresses the chunks exactly as the build
did and writes the full DLL; the T2ME hash of the finished blob must match, so any mismatch is caught.

  build side:  plan(stage_dir)  -> which staged files are (a) identical to the game's file (dropped when the archive is
               always mounted) or (c) edited copies (patched)
               emit(full_dll, stage_dir, plan, slim_dll, dat)  -> writes the slim DLL and the .dat
  CLI:         python slim.py make  <full.dll> <stage_dir> <slim.dll> <slim.dat>     (plan + emit, for an existing build)
               python slim.py check <full.dll> <slim.dll> <slim.dat>                 (run t2r-slim.exe, cmp with the full DLL)
Single threaded, streaming, low memory: the biggest file is held twice (original + patched, ~330 MB) while it is patched.
Environment: T2R_GAME_DIR (default C:\\Games\\The Last of Us Part II Remastered, read only), T2R_SLIM_INDEX (re/game-files.tsv:
archive, path, size per game file), T2R_SLIM_ZSTD (zstd.exe), T2R_SLIM_TMP (scratch dir), T2R_SLIM_LEVEL (zstd level).
"""
import collections
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import sys as _t2s, os as _t2o; _t2s.path.append(_t2o.path.normpath(_t2o.path.join(_t2o.path.dirname(_t2o.path.abspath(__file__)), ".."))); import t2rtmp  # noqa: E402,F401  (temp hygiene: tools/t2rtmp.py)
import threading
import time
import zlib

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

DEFAULT_GAME = r"C:\Games\The Last of Us Part II Remastered"
MAIN = "build/pc/main"
# archives the engine always has mounted: a staged file identical to one of theirs can simply be left out of the mod archive
# (the engine finds the game's own copy). Anything else (sp-common, world-*: not mounted in the MP levels, that is why the mod
# carries copies at all) is rebuilt on the player's PC instead, identical files with an empty patch.
ALWAYS_MOUNTED = ("common.psarc", "core.psarc", "bin.psarc")
DSAR_CHUNK = 0x40000
LZ4_LEVEL = 12
PSARC_PACK = 1 << 20


def _first_existing(*c):
    for p in c:
        if p and os.path.exists(p):
            return p
    return c[0]


def game_dir():
    return os.environ.get("T2R_GAME_DIR") or DEFAULT_GAME


def index_path():
    return _first_existing(os.environ.get("T2R_SLIM_INDEX"), os.path.join(ROOT, "re", "game-files.tsv"))


def zstd_exe():
    return _first_existing(os.environ.get("T2R_SLIM_ZSTD"), os.path.join(TOOLS, "win", "ext", "msys64", "usr", "bin", "zstd.exe"),
                           os.path.join(TOOLS, "win", "ext", "msys64", "ucrt64", "bin", "zstd.exe"))


def cache_dir():
    d = os.environ.get("T2R_SLIM_CACHE") or os.path.join(TOOLS, "local", "build-cache", "slim")
    os.makedirs(d, exist_ok=True)
    return d


def tmp_dir():
    d = os.environ.get("T2R_SLIM_TMP") or os.path.join(tempfile.gettempdir(), "t2r-slim")
    os.makedirs(d, exist_ok=True)
    return d


def lower_priority():
    if os.name == "nt":
        import ctypes
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)    # BELOW_NORMAL


# ------------------------------------------------------------------ the game's files
def load_index(tsv=None):
    idx = collections.defaultdict(list)
    with open(tsv or index_path(), encoding="utf8") as f:
        for ln in f:
            a, p, s = ln.rstrip("\n").split("\t")
            idx[p].append((a, int(s)))
    return idx


_opened = {}


def _arch(gamedir, a):
    import dsar_list
    if a not in _opened:
        if len(_opened) > 2:
            _opened.clear()
        _opened[a] = dsar_list.open_files(os.path.join(gamedir, *MAIN.split("/"), a))
    return _opened[a]


def game_stream(gamedir, a, p):
    """the game's file `p` of archive `a`, as a generator of byte pieces (never the whole file in memory)"""
    d, ent, bsizes, bsize = _arch(gamedir, a)
    bi, usz, off = ent[p]
    got = 0
    while got < usz:
        cs = bsizes[bi] or bsize
        blk = d.read(off, cs)
        want = min(bsize, usz - got)
        yield blk if cs == want else zlib.decompress(blk)
        got += want
        off += cs
        bi += 1
        if len(d.cache) > 8:
            d.cache.clear()


def file_sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def game_sha1(gamedir, a, p, size):
    """sha1 of a game file; remembered by (archive, path, size, archive mtime)"""
    cj = os.path.join(cache_dir(), "game-sha1.json")
    try:
        cache = json.load(open(cj))
    except (OSError, ValueError):
        cache = {}
    key = f"{a}|{p}|{size}|{os.stat(os.path.join(gamedir, *MAIN.split('/'), a)).st_mtime_ns}"
    if key not in cache:
        h = hashlib.sha1()
        for b in game_stream(gamedir, a, p):
            h.update(b)
        cache[key] = h.hexdigest()
        json.dump(cache, open(cj, "w"))
    return cache[key]


# ------------------------------------------------------------------ plan: what is identical, what is an edited copy
def walk_stage(stage):
    for dp, _, fn in os.walk(stage):
        for f in fn:
            fp = os.path.join(dp, f)
            yield os.path.relpath(fp, stage).replace("\\", "/"), fp


def classify(stage, gamedir=None, idx=None, log=print):
    """[{path, size, kind: 'identical' | 'edited', arch, base_size, sha_new, sha_base}] for the staged files that exist in the game
    (same path); files without a game counterpart are new content and not listed."""
    gamedir = gamedir or game_dir()
    idx = idx or load_index()
    out = []
    for rel, fp in sorted(walk_stage(stage)):
        if rel.startswith("_t2r") or rel not in idx:
            continue
        size = os.path.getsize(fp)
        cands = idx[rel]
        a, bsz = next(((a, s) for a, s in cands if s == size), cands[0])        # prefer the archive with the same size
        sn = file_sha1(fp)
        sb = game_sha1(gamedir, a, rel, bsz)
        out.append(dict(path=rel, size=size, kind="identical" if sn == sb else "edited", arch=a, base_size=bsz, sha_new=sn, sha_base=sb))
    return out


def _peak_ws(p):
    try:
        import ctypes
        from ctypes import wintypes

        class PMC(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [(n, ctypes.c_size_t) for n in (
                "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage", "QuotaPagedPoolUsage",
                "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage")]
        pmc = PMC()
        pmc.cb = ctypes.sizeof(pmc)
        ctypes.windll.psapi.GetProcessMemoryInfo(wintypes.HANDLE(int(p._handle)), ctypes.byref(pmc), pmc.cb)
        return pmc.PeakWorkingSetSize
    except Exception:
        return 0


def level_for(size):
    if os.environ.get("T2R_SLIM_LEVEL"):
        return int(os.environ["T2R_SLIM_LEVEL"])
    return 19 if size <= (24 << 20) else 12          # -19 --long=30 on a 160 MB level pak wants >1 GB of RAM: not for the big ones


def make_delta(e, gamedir=None, log=print):
    """the zstd patch (original -> staged file) of an 'edited' entry, cached by content hashes; returns its path"""
    gamedir = gamedir or game_dir()
    level = level_for(e["size"])
    dst = os.path.join(cache_dir(), f"{e['sha_new']}-{e['sha_base']}-L{level}.zst")
    if os.path.exists(dst):
        return dst
    stage_file = e["fp"]
    base = os.path.join(tmp_dir(), "base-%d-%d.bin" % (os.getpid(), threading.get_ident()))      # (one per worker: emit runs several patches at once)
    with open(base, "wb") as f:
        for b in game_stream(gamedir, e["arch"], e["path"]):
            f.write(b)
    tmp = dst + ".tmp"
    cmd = [zstd_exe(), f"-{level}", "-T1", "--long=30", f"--patch-from={base}", stage_file, "-o", tmp, "-f", "-q", "--no-progress"]
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    err = p.communicate()[1]
    peak = _peak_ws(p)
    os.remove(base)
    if p.returncode != 0:
        raise SystemExit(f"zstd failed for {e['path']}: {err.decode(errors='replace')}")
    os.replace(tmp, dst)
    log(f"  slim: patch for {e['path']}: {e['size'] / 1e6:.1f} MB -> {os.path.getsize(dst) / 1e3:.0f} KB (zstd -{level}, peak {peak / 1e6:.0f} MB)")
    return dst


def plan(stage, gamedir=None, log=print):
    """classify the stage; the result's 'drop' = paths to leave out of the archive, 'entries' = the classified list"""
    lower_priority()
    gamedir = gamedir or game_dir()
    if not os.path.isdir(os.path.join(gamedir, *MAIN.split("/"))):
        raise SystemExit(f"T2R_SLIM: game folder not found: {gamedir} (set T2R_GAME_DIR)")
    ents = classify(stage, gamedir, log=log)
    for e in ents:
        e["fp"] = os.path.join(stage, e["path"])
    drop = [e["path"] for e in ents if e["kind"] == "identical" and e["arch"] in ALWAYS_MOUNTED]
    ident = [e for e in ents if e["kind"] == "identical"]
    log(f"slim plan: {len(ents)} staged files exist in the game: {len(ident)} identical "
        f"({sum(e['size'] for e in ident) / 1e6:.1f} MB, {len(drop)} dropped outright), "
        f"{len(ents) - len(ident)} edited ({sum(e['size'] for e in ents if e['kind'] == 'edited') / 1e6:.1f} MB)")
    return dict(entries=ents, drop=drop, gamedir=gamedir, stage=stage)


# ------------------------------------------------------------------ the T2ME blob inside the finished DLL
def _find_blob(path):
    import pefile
    pe = pefile.PE(path, fast_load=True)
    try:
        pe.parse_data_directories([pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"]])
        for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
            if t.id != 10:
                continue
            for n in t.directory.entries:
                if n.id == 1001:
                    ent = n.directory.entries[0].data.struct
                    return pe.get_offset_from_rva(ent.OffsetToData)
    finally:
        pe.close()
    raise SystemExit("RCDATA 1001 (T2ME blob) not found")


class Blob:
    """read access to the decrypted DSAR of a finished DLL (mmap; decrypts only what is asked for)"""

    def __init__(self, path):
        import mmap
        import numpy as np
        self.np = np
        self.f = open(path, "rb")
        self.mm = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        if self.mm[:4] == b"T2RD":          # T2R_PACK_FILE data file (pack_file.py): size + FNV + payload offset in the header
            ver, size, h = struct.unpack_from("<IQQ", self.mm, 4)
            if ver != 1:
                raise SystemExit("t2r-data.bin: unknown version %d" % ver)
            hdr, delta = struct.unpack_from("<QQ", self.mm, 0x28)          # payload at hdr + delta (a converted DLL's file keeps its old delta)
            self.off, self.size, self.hash = hdr + delta, size, h
        else:
            off = _find_blob(path)
            magic, ver, size, h = struct.unpack_from("<4sIQQ", self.mm, off)
            if magic != b"T2ME" or ver != 4:
                raise SystemExit("not a T2ME blob")
            self.off, self.size, self.hash = off + 0x18, size, h
        n = struct.unpack_from("<I", self.dec(8, 12), 0)[0]
        d = self.dec(0x28, 0x28 + n * 0x20)
        self.n = n
        self.ent = [struct.unpack_from("<QIIQQ", d, i * 0x20) for i in range(n)]          # off, usz, csz, tag, uend
        self.ustart = []
        u = 0
        for e in self.ent:
            self.ustart.append(u)
            u += e[1]
        self.total = u

    def dec(self, a, b):
        """decrypted payload bytes [a, b) (the tail after the last whole qword is not encrypted)"""
        import t2r_pack
        np = self.np
        a0 = a & ~7
        end = min((b + 7) & ~7, self.size)
        raw = self.mm[self.off + a0:self.off + end]
        nq = len(raw) // 8
        out = bytearray()
        if nq and a0 < (self.size // 8) * 8:
            nq = min(nq, (self.size // 8) - a0 // 8)
            out += t2r_pack._keystream_chunk(np.frombuffer(raw[:nq * 8], dtype="<u8"), a0 // 8).tobytes()
        out += raw[len(out):]
        return bytes(out[a - a0:b - a0])

    def chunk(self, i):
        import lz4.block
        off, usz, csz, _, _ = self.ent[i]
        raw = self.dec(off, off + csz)
        if csz == usz:
            return raw
        return lz4.block.decompress(raw, uncompressed_size=usz)

    def read(self, pos, n):
        import bisect
        out = bytearray()
        i = bisect.bisect_right(self.ustart, pos) - 1
        while len(out) < n:
            c = self.chunk(i)
            a = pos + len(out) - self.ustart[i]
            out += c[a:a + n - len(out)]
            i += 1
        return bytes(out)

    def close(self):
        self.mm.close()
        self.f.close()


def psarc_locate(blob, md5s):
    """{md5: (psarc offset, size)} from the PSARC table of contents at the start of the blob"""
    h = blob.read(0, 32)
    assert h[:4] == b"PSAR", h[:4]
    toc_len, esz, n = struct.unpack_from(">III", h, 12)
    assert esz == 30
    toc = blob.read(0, toc_len)
    want = set(md5s)
    res = {}
    for i in range(1, n):
        e = toc[32 + i * 30:32 + (i + 1) * 30]
        if e[:16] in want:
            res[e[:16]] = (int.from_bytes(e[25:30], "big"), int.from_bytes(e[20:25], "big"))
    return res


# ------------------------------------------------------------------ the xz pack stream (T2SLIM02)
PACK_DICT = 128 << 20                    # xz dictionary: the helper's decoder allocates this much (154 MB of 528 MB with 256 MiB: only 1.7 % better)
PACK_FILTERS = None                      # set below (needs lzma at import time only when used)


# T2R_FAST_PACK=1 (A/B test builds only; release builds never set it): a lighter xz preset (6, nice_len 64, 64 MiB dictionary) cut into
# blocks that xz compresses on cpu_count-2 threads (T2R_SLIM_XZ_THREADS overrides); a different, slightly bigger .dat than the release one.
FAST_PACK = os.environ.get("T2R_FAST_PACK") == "1"
if FAST_PACK:
    PACK_DICT = 64 << 20


def _xz_threads():
    t = int(os.environ.get("T2R_SLIM_XZ_THREADS", "0") or 0)
    return t if t or not FAST_PACK else max(2, (os.cpu_count() or 4) - 2)


def pack_filters():
    import lzma
    lc, lp, pb = (int(x) for x in os.environ.get("T2R_SLIM_XZ_LCLPPB", "1,3,3").split(","))
    if FAST_PACK:
        return [{"id": lzma.FILTER_LZMA2, "preset": 6, "dict_size": PACK_DICT, "lc": lc, "lp": lp, "pb": pb,
                 "nice_len": 64, "mf": lzma.MF_BT4, "depth": 0}]
    return [{"id": lzma.FILTER_LZMA2, "preset": 9 | lzma.PRESET_EXTREME, "dict_size": PACK_DICT, "lc": lc, "lp": lp, "pb": pb,
             "nice_len": 273, "mf": lzma.MF_BT4, "depth": 1000}]


def xz_single(path, sha, log=print):
    """size of one staged file alone as xz (an upper bound of what it adds to the solid pack stream); cached by content hash"""
    cj = os.path.join(cache_dir(), f"xz1-{sha}{'-fast' if FAST_PACK else ''}.size")
    try:
        return int(open(cj).read())
    except (OSError, ValueError):
        pass
    import lzma
    c = lzma.LZMACompressor(format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64, filters=pack_filters())
    n = 0
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 22), b""):
            n += len(c.compress(b))
    n += len(c.flush())
    with open(cj, "w") as f:
        f.write(str(n))
    return n


def xz_cli():
    for d in (("ucrt64", "bin"), ("usr", "bin")):
        p = os.path.join(TOOLS, "win", "ext", "msys64", *d, "xz.exe")
        if os.path.exists(p):
            return p
    raise SystemExit("T2R_SLIM_XZ_THREADS: xz.exe not found in tools/win/ext/msys64")


def _pack_mt(files, dst, threads, block_mb, log):
    """OPT-IN (T2R_SLIM_XZ_THREADS=N, N > 1): the same LZMA2 settings, but cut into independent blocks of T2R_SLIM_XZ_BLOCK_MB (default
    96) MiB that xz compresses on N threads (one .xz stream, several blocks: liblzma's stream decoder, i.e. t2r-slim.exe, reads it
    unchanged). Several times faster, slightly bigger (blocks do not share a dictionary) - and a DIFFERENT .dat than the default."""
    lc, lp, pb = (int(x) for x in os.environ.get("T2R_SLIM_XZ_LCLPPB", "1,3,3").split(","))
    cmd = [xz_cli(), "-c", f"--threads={threads}", f"--block-size={block_mb << 20}", "--check=crc64",
           (f"--lzma2=preset=6,dict={PACK_DICT},lc={lc},lp={lp},pb={pb},nice=64,mf=bt4,depth=0" if FAST_PACK else
            f"--lzma2=preset=9e,dict={PACK_DICT},lc={lc},lp={lp},pb={pb},nice=273,mf=bt4,depth=1000"), "-"]
    with open(dst, "wb") as fo:
        p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=fo)
        try:
            for _, fp, _ in files:
                with open(fp, "rb") as f:
                    for b in iter(lambda: f.read(1 << 22), b""):
                        p.stdin.write(b)
        finally:
            p.stdin.close()
        if p.wait():
            raise SystemExit(f"xz failed (rc {p.returncode})")


def make_pack(files, log=print):
    """files: [(path, fp, sha1)] already in stream order -> (stream file path, [offset of each file]); cached by the content hashes.
    One solid xz stream, strictly in this order: the helper decodes it sequentially."""
    import lzma
    mt = _xz_threads()
    blk = int(os.environ.get("T2R_SLIM_XZ_BLOCK_MB", "96") or 96)
    key = hashlib.sha1(("\n".join(f"{p}:{s}" for p, _, s in files) + f"|{PACK_DICT}|{os.environ.get('T2R_SLIM_XZ_LCLPPB', '1,3,3')}|v1"
                        + (f"|mt{blk}" if mt > 1 else "") + ("|fast" if FAST_PACK else "")).encode()).hexdigest()       # (the thread count does not change the bytes, the block size does)
    dst = os.path.join(cache_dir(), f"pack-{key}.xz")
    offs, pos = [], 0
    for _, fp, _ in files:
        offs.append(pos)
        pos += os.path.getsize(fp)
    if os.path.exists(dst):
        return dst, offs, pos
    t0 = time.time()
    tmp = dst + ".tmp"
    if mt > 1:
        _pack_mt(files, tmp, mt, blk, log)
    else:
        c = lzma.LZMACompressor(format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64, filters=pack_filters())
        with open(tmp, "wb") as fo:
            for _, fp, _ in files:
                with open(fp, "rb") as f:
                    for b in iter(lambda: f.read(1 << 22), b""):
                        fo.write(c.compress(b))
            fo.write(c.flush())
    os.replace(tmp, dst)
    log(f"  slim: pack stream {pos / 1e6:.0f} MB -> {os.path.getsize(dst) / 1e6:.1f} MB (xz, {len(files)} files, {time.time() - t0:.0f} s)")
    return dst, offs, pos


# ------------------------------------------------------------------ emit
DAT_MAGIC = b"T2SLIM02"
HEAD_FMT = "<8sQQQQIIQQQ"
REC_FMT = "<64s16s96sQQQQQIIIIQ"


def emit(full_dll, pl, slim_dll, dat, min_gain=1.25, log=print, pack=None, extras=()):
    """Write the slim DLL (the full one with the chunk data of every staged file zeroed) and its .dat.
    pack (default: T2R_SLIM_PACK != 0): files without a game counterpart, and edited copies whose patch is not clearly smaller than
    the file as xz, travel in ONE solid xz stream (kind 1) instead of staying in the DLL as lz4 (XOR-encrypted: incompressible).
    extras: [(path relative to the game folder, local file)] shipped in the same stream (kind 2) and written there by the helper
    (tlou-ii-mp/sidbase.bin: 80 MB, 30.9 MB as zip deflate, 18.1 MB as xz); only with pack."""
    import bisect
    import t2r_pack
    lower_priority()
    if pack is None:
        pack = os.environ.get("T2R_SLIM_PACK", "1") != "0"
    dropset = set(pl["drop"])
    ents = [e for e in pl["entries"] if e["path"] not in dropset]
    _t = [time.perf_counter()]

    def lap(what):
        now = time.perf_counter()
        log(f"  slim timing: {what}: {now - _t[0]:.1f} s")
        _t[0] = now
    blob = Blob(full_dll)
    try:
        if t2r_pack.fnv_payload(blob.mm[blob.off:blob.off + blob.size]) != blob.hash:
            raise SystemExit("slim: the T2ME hash of the input DLL is wrong")
        game_paths = {e["path"] for e in pl["entries"]}
        new_files = []                                   # (path, fp): staged files without a game counterpart
        if pack:
            for rel, fp in sorted(walk_stage(pl["stage"])):
                if rel.startswith("_t2r") or rel in game_paths or rel in dropset:
                    continue
                new_files.append((rel, fp))
        md5 = {p: hashlib.md5(p.encode()).digest() for p in [e["path"] for e in ents] + [p for p, _ in new_files]}
        loc = psarc_locate(blob, md5.values())
        for rel, _ in [x for x in new_files if md5[x[0]] not in loc]:
            log(f"  slim: {rel} is staged but not in the blob (not in the manifest?): ignored")
        new_files = [x for x in new_files if md5[x[0]] in loc]

        def chunks_of(path, fp, usz_expected):
            """the DSAR entries of a staged file; the blob must really hold it"""
            m = md5[path]
            if m not in loc:
                raise SystemExit(f"slim: {path} is not in the blob")
            poff, usz = loc[m]
            assert usz == usz_expected, (path, usz, usz_expected)
            c0 = bisect.bisect_left(blob.ustart, poff)
            if c0 >= blob.n or blob.ustart[c0] != poff:
                raise SystemExit(f"slim: {path} does not start at a DSAR entry")
            ci = []
            c = c0
            while c < blob.n and blob.ustart[c] < poff + usz:
                ci.append(c)
                c += 1
            if sum(blob.ent[c][1] for c in ci) != usz or any(blob.ent[c][1] != DSAR_CHUNK for c in ci[:-1]):
                raise SystemExit(f"slim: {path}: unexpected chunk layout")
            with open(fp, "rb") as f:
                for c in ci:
                    if blob.chunk(c) != f.read(DSAR_CHUNK):
                        raise SystemExit(f"slim: blob chunk {c} of {path} differs from the staged file")
            return ci

        if pack:                                          # the single-file xz estimates of the edited files, on all cores (cached)
            import concurrent.futures
            todo = [e for e in ents if e["kind"] != "identical"]
            with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, (os.cpu_count() or 4) - 1)) as ex:
                list(ex.map(lambda e: xz_single(e["fp"], e["sha_new"], log), todo))
        lap("blob open + staged-file xz estimates")
        # the zstd patches of every edited file at once (separate -T1 processes; cached by content hash, so the loop below only finds them):
        # same files as before, only no longer one after the other. T2R_SLIM_DELTA_JOBS = workers (default 6: -19 --long=30 wants ~0.5 GB each)
        import concurrent.futures
        jobs = int(os.environ.get("T2R_SLIM_DELTA_JOBS", "6"))
        todo_d = [e for e in ents if e["kind"] != "identical"]
        if jobs > 1 and len(todo_d) > 1:
            with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
                list(ex.map(lambda e: make_delta(e, pl["gamedir"], log), todo_d))
        lap("zstd patches (parallel prefetch)")
        recs, chunks, ranges = [], [], []
        saved = kept = n_pack_from_edit = 0
        pack_list = []                                    # (path, fp, sha1)
        edited_pack = []
        for e in sorted(ents, key=lambda e: e["path"]):
            ci = chunks_of(e["path"], e["fp"], e["size"])
            csz_total = sum(blob.ent[c][2] for c in ci)
            if e["kind"] == "identical":
                dl, dpath = 0, None
            else:
                dpath = make_delta(e, pl["gamedir"], log)
                dl = os.path.getsize(dpath)
            if pack and e["kind"] != "identical" and dl >= 0.95 * xz_single(e["fp"], e["sha_new"], log):
                # (solid xz of the whole stream shares even more between files) the patch is no real gain over the file alone
                pack_list.append((e["path"], e["fp"], e["sha_new"]))
                n_pack_from_edit += 1
                log(f"  slim: packing {e['path']} instead of patching it (patch {dl / 1e3:.0f} KB)")
                continue
            if not pack and dl * min_gain >= csz_total:
                kept += csz_total
                log(f"  slim: keeping {e['path']} ({csz_total / 1e3:.0f} KB in the DLL, patch {dl / 1e3:.0f} KB)")
                continue
            raw_fnv = t2r_pack.fnv_payload(open(e["fp"], "rb").read())
            recs.append(dict(kind=0, path=e["path"], arch=e["arch"], md5=md5[e["path"]], base_size=e["base_size"], usz=e["size"], dl=dl,
                             dpath=dpath, raw_fnv=raw_fnv, chunk0=len(chunks), nchunks=len(ci), pack_pos=0))
            for c in ci:
                off, cu, cs, _, _ = blob.ent[c]
                chunks.append((off, cu, cs))
                ranges.append((blob.off + off, cs))
            saved += csz_total - dl
        n_patched = len(recs)
        lap("chunk verification vs staged files + patch records")
        if pack:
            for rel, fp in new_files:
                pack_list.append((rel, fp, file_sha1(fp)))
            pack_list.sort(key=lambda t: (os.path.splitext(t[0])[1], t[0]))        # similar content next to each other
            extra_rels = {rel for rel, _ in extras}
            stream_list = pack_list + [(rel, fp, file_sha1(fp)) for rel, fp in extras]        # (extras last)
            pstream, offs, pusz = make_pack(stream_list, log) if stream_list else (None, [], 0)
            lap("solid xz pack stream")
            for (rel, fp, _), poff_ in zip(stream_list, offs):
                usz = os.path.getsize(fp)
                raw_fnv = t2r_pack.fnv_payload(open(fp, "rb").read())
                if rel in extra_rels:
                    recs.append(dict(kind=2, path=rel, arch="", md5=bytes(16), base_size=0, usz=usz, dl=0, dpath=None, raw_fnv=raw_fnv,
                                     chunk0=len(chunks), nchunks=0, pack_pos=poff_))
                    continue
                ci = chunks_of(rel, fp, usz)
                recs.append(dict(kind=1, path=rel, arch="", md5=md5[rel], base_size=0, usz=usz, dl=0, dpath=None, raw_fnv=raw_fnv,
                                 chunk0=len(chunks), nchunks=len(ci), pack_pos=poff_))
                for c in ci:
                    off, cu, cs, _, _ = blob.ent[c]
                    chunks.append((off, cu, cs))
                    ranges.append((blob.off + off, cs))
                saved += sum(blob.ent[c][2] for c in ci)
        else:
            if extras:
                raise SystemExit("slim: extras need the pack stream (T2R_SLIM_PACK=0 is set)")
            pstream, pusz = None, 0
        # .dat
        hdr = struct.calcsize(HEAD_FMT)
        base = hdr + struct.calcsize(REC_FMT) * len(recs) + struct.calcsize("<QII") * len(chunks)
        pos = base
        recb = bytearray()
        for r in recs:
            doff = 0
            if r["dl"]:
                doff = pos
                pos += r["dl"]
            recb += struct.pack(REC_FMT, r["arch"].encode(), r["md5"], r["path"].encode(), r["base_size"], r["usz"], doff, r["dl"], r["raw_fnv"],
                                r["chunk0"], r["nchunks"], r["kind"], 0, r["pack_pos"])
        lap("pack records (fnv of every packed file)")
        pack_off, pack_len = pos, (os.path.getsize(pstream) if pstream else 0)
        chb = b"".join(struct.pack("<QII", *c) for c in chunks)
        dll_size = os.path.getsize(full_dll)
        with open(dat, "wb") as f:
            f.write(struct.pack(HEAD_FMT, DAT_MAGIC, blob.off, blob.size, blob.hash, dll_size, len(recs), len(chunks), pack_off if pack_len else 0, pack_len, pusz))
            f.write(recb)
            f.write(chb)
            for r in recs:
                if r["dl"]:
                    with open(r["dpath"], "rb") as d:
                        f.write(d.read())
            if pack_len:
                with open(pstream, "rb") as d:
                    for b in iter(lambda: d.read(1 << 22), b""):
                        f.write(b)
        lap(".dat write")
        # slim DLL: streaming copy with the ranges zeroed
        ranges.sort()
        with open(full_dll, "rb") as fi, open(slim_dll, "wb") as fo:
            pos = 0
            for a, n in ranges + [(dll_size, 0)]:
                while pos < a:
                    k = min(a - pos, 1 << 22)
                    fo.write(fi.read(k))
                    pos += k
                if n:
                    fi.seek(a + n)
                    fo.write(b"\0" * n)
                    pos = a + n
        lap("slim DLL write")
        datsz = os.path.getsize(dat)
        n_pack = sum(1 for r in recs if r["kind"] == 1)
        log(f"slim: {n_patched} files patched, {n_pack} packed + {len(recs) - n_patched - n_pack} extra ({n_pack_from_edit} of them edited game files), "
            f"DLL chunks zeroed: {sum(n for _, n in ranges) / 1e6:.1f} MB, .dat {datsz / 1e6:.1f} MB (patches {sum(r['dl'] for r in recs) / 1e6:.1f}, "
            f"pack {pack_len / 1e6:.1f}); net {(sum(n for _, n in ranges) - datsz) / 1e6:.1f} MB saved in download size ({kept / 1e6:.1f} MB kept as is)")
        return dict(patched=n_patched, packed=n_pack, extras=len(recs) - n_patched - n_pack, zeroed=sum(n for _, n in ranges), dat=datsz)
    finally:
        blob.close()


# ------------------------------------------------------------------ helper exe
def build_helper(out_exe=None):
    src = os.path.join(ROOT, "release", "client", "slim", "t2r-slim.c")
    out_exe = out_exe or os.path.join(os.path.dirname(src), "t2r-slim.exe")
    g = os.environ.get("T2R_SLIM_GCCDIR") or os.path.join(TOOLS, "win", "ext", "msys64", "ucrt64", "bin")     # (msys2 ucrt64 bin: gcc + static zstd / lz4 / lzma / zlib)
    env = dict(os.environ, PATH=g + os.pathsep + os.environ["PATH"])
    subprocess.run([os.path.join(g, "gcc.exe"), "-std=gnu11", "-O2", "-s", "-static", "-municode", "-DLZMA_API_STATIC", "-o", out_exe, src, "-lzstd", "-llz4", "-llzma", "-lz"],
                   check=True, env=env)
    return out_exe


def check(full_dll, slim_dll, dat, gamedir=None, exe=None):
    """run the helper on the slim DLL and compare with the full one (bytes)"""
    exe = exe or os.path.join(ROOT, "release", "client", "slim", "t2r-slim.exe")
    out = os.path.join(tmp_dir(), "rebuilt.dll")
    # (the build's check must stay read-only on the game folder: no extra files (sidbase.bin) written there)
    r = subprocess.run([exe, slim_dll, dat, gamedir or game_dir(), out], env=dict(os.environ, T2R_SLIM_SKIP_EXTRAS="1"))
    if r.returncode:
        raise SystemExit(f"t2r-slim exited with {r.returncode}")
    with open(full_dll, "rb") as a, open(out, "rb") as b:
        while True:
            x, y = a.read(1 << 22), b.read(1 << 22)
            if x != y:
                raise SystemExit("slim check: the rebuilt DLL differs from the full DLL")
            if not x:
                break
    os.remove(out)
    print("slim check: rebuilt DLL is byte-identical to the full DLL")


if __name__ == "__main__":
    cmd, *a = sys.argv[1:]
    if cmd == "make":
        full, stage, slim_dll, dat = a
        emit(full, plan(stage), slim_dll, dat)
    elif cmd == "check":
        check(*a)
    else:
        raise SystemExit(__doc__)
