"""Turn a FINISHED in-image winmm.dll (payload in `.t2rc`) into the T2R_PACK_FILE form without a data build:
winmm.dll with the 64 KiB stub + the data-file cave, and t2r-maps/main/t2r-data.bin holding the same payload bytes.

The new winmm.dll is rebuilt from the base DLL with build_dll's own patch chain (the patch list is read from build_dll.py's source, so it
cannot drift); the payload is copied from the finished DLL through mmap (no 1.8 GB copy in memory). A P1-side build keeps its part1.dat:
the payload offset inside its 64 KiB view (delta = FO % 64 KiB) is carried into the data file, so part1.dat still maps right behind it.
The report compares every section the two images share by name and size, so a drift of the chain shows.

  python pack_file_convert.py <finished winmm.dll> <out dir> [--port N]
    -> <out dir>/winmm.dll, <out dir>/t2r-maps/main/t2r-data.bin    (deploy both; part1.dat unchanged)
"""
import argparse
import ast
import mmap
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, TOOLS)
import pack_file  # noqa: E402
import t2r_pack  # noqa: E402


def chain_patch_list():
    """the winmm_patches.apply name list of build_dll.build()'s finish(), read from the source"""
    src = open(os.path.join(HERE, "build_dll.py"), encoding="utf-8").read()
    for node in ast.walk(ast.parse(src)):
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and node.func.attr == "apply" and \
                isinstance(node.func.value, ast.Name) and node.func.value.id == "winmm_patches" and len(node.args) == 2 and isinstance(node.args[1], ast.List):
            return [e.value for e in node.args[1].elts]
    raise SystemExit("convert: winmm_patches.apply list not found in build_dll.py")


def sections(d):
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    tab = pe + 24 + struct.unpack_from("<H", d, pe + 20)[0]
    out = {}
    for i in range(nsec):
        b = tab + 40 * i
        nm = bytes(d[b:b + 8]).rstrip(b"\0").decode()
        vs, va, rs, rp = struct.unpack_from("<IIII", d, b + 8)
        out[nm] = (va, vs, rs, rp)
    return out, struct.unpack_from("<I", d, pe + 24 + 56)[0]


def code_diff_check(old, so, new, sn):
    """the rebuilt image's code must be the finished image's code: every differing byte has to be a 32-bit field that points at a section
    that moved (an absolute address shifted by the move of the sections after .rsrc) or a rel32 whose site moved with its section; the one
    allowed rewrite is the loader hook (jmp to the data-file cave in place of p1_side_map's jmp). Anything else = the chain drifted: stop."""
    moved = [n for n in (".t2rx", ".t2r8", ".t2rstub") if n in so and n in sn]
    if not moved:
        raise SystemExit("convert: no code sections after .rsrc to compare")
    D = so[moved[0]][0] - sn[moved[0]][0]
    for n in moved:
        if so[n][0] - sn[n][0] != D:
            raise SystemExit(f"convert: section {n} moved by {so[n][0] - sn[n][0]:#x}, not {D:#x}")
    hook = set(range(pack_file.HOOK_VMA - pack_file.IMAGE_BASE, pack_file.HOOK_VMA - pack_file.IMAGE_BASE + 6))
    import patch_winmm                               # the server address sites (--port / another target than the finished DLL's)
    tva, _tvs, _trs, trp = so[".text"]
    target = set()
    for o, n in [(o, 4) for o in patch_winmm.IP_OFFS] + [(o, 2) for o in patch_winmm.PORT_OFFS]:
        target.update(range(o - trp + tva, o - trp + tva + n))
    for nm in (".text",) + tuple(moved):
        va, vs, rs, rp = so[nm]
        vb, _, rsb, rpb = sn[nm]
        if rs != rsb:
            raise SystemExit(f"convert: section {nm} raw size {rs:#x} -> {rsb:#x}")
        xa, xb = bytes(old[rp:rp + rs]), bytes(new[rpb:rpb + rs])
        shift = va - vb
        cov, kinds, bad = set(), {"abs": 0, "rel": 0, "hook": 0, "target": 0}, []
        for k in (i for i in range(rs) if xa[i] != xb[i]):
            if k in cov:
                continue
            if nm == ".text" and va + k in hook:
                kinds["hook"] += 1
                continue
            if nm == ".text" and va + k in target:
                kinds["target"] += 1
                continue
            hit = None
            for s in range(max(0, k - 3), min(k, rs - 4) + 1):
                oa, ob = struct.unpack_from("<I", xa, s)[0], struct.unpack_from("<I", xb, s)[0]
                if (oa - ob) & 0xFFFFFFFF == D & 0xFFFFFFFF:
                    hit = "abs"
                elif (oa - ob + shift) & 0xFFFFFFFF == 0:
                    hit = "rel"
                if hit:
                    cov.update(range(s, s + 4))
                    kinds[hit] += 1
                    break
            if not hit:
                bad.append(k)
        print(f"  section {nm:9s} raw {rs:#x}: {kinds['abs']} moved addresses, {kinds['rel']} moved rel32, {kinds['hook']} hook bytes, {kinds['target']} server address bytes, "
              f"{len(bad)} unexplained")
        if bad:
            raise SystemExit(f"convert: {nm} differs at {', '.join(hex(pack_file.IMAGE_BASE + va + k) for k in bad[:12])}: the patch chain "
                             "is not the one the finished DLL was built with")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dll")
    ap.add_argument("out")
    ap.add_argument("--port", type=int, help="server port (default: the finished DLL's)")
    ap.add_argument("--base", default=os.environ.get("T2R_BASE_DLL") or next((c for c in (
        os.path.join(TOOLS, "local", "winmm-nocave-127.0.0.1-7001.dll"),
        os.path.join(os.environ.get("T2R_MAIN_ROOT", os.path.join(TOOLS, "..")), "tools", "local", "winmm-nocave-127.0.0.1-7001.dll")) if os.path.isfile(c)), None))
    a = ap.parse_args()
    import patch_winmm
    import winmm_patches
    import ext_loader
    import stubpool
    import shrink_rsrc
    f = open(a.dll, "rb")
    mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    secs, soi_old = sections(mm[:0x1000])
    if ".t2rc" not in secs:
        raise SystemExit("convert: %s has no .t2rc (already converted, or a small in-place build)" % a.dll)
    # the payload, located like the loader does (resource 1001 -> its section)
    hdr = bytes(mm[:0x1000])
    head = bytearray(mm[:secs[".t2rc"][3]])         # everything before the payload section's raw data (headers, .text ... .rsrc) for pefile
    pe_off = struct.unpack_from("<I", hdr, 0x3C)[0]
    import pefile
    pe = pefile.PE(data=bytes(head) + bytes(0x1000), fast_load=True)
    pe.parse_data_directories([pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"]])
    ent = None
    for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        if t.id == 10:
            for n in t.directory.entries:
                if n.id == 1001:
                    ent = n.directory.entries[0].data.struct
    va, vs, rs, rp = secs[".t2rc"]
    assert ent and va <= ent.OffsetToData < va + rs, "convert: resource 1001 is not in .t2rc"
    t2me_off = rp + ent.OffsetToData - va
    magic, ver, size1, fnv = struct.unpack_from("<4sIQQ", mm, t2me_off)
    assert magic == b"T2ME" and ver == 4, (magic, ver)
    fo = t2me_off + 0x18
    payload = memoryview(mm)[fo:fo + size1]
    delta = fo % 0x10000
    size2, bid = 0, None
    if ".t2rps" in secs:                             # P1-side build (p1_side_map.py, descriptor version 1): part1.dat config
        prp = secs[".t2rps"][3]
        m2, v2 = struct.unpack_from("<II", mm, prp)
        assert m2 == 0x44533150 and v2 == 1, "convert: .t2rps is not a p1_side_map descriptor"
        size2 = struct.unpack_from("<Q", mm, prp + 0x60)[0]
        bid = bytes(mm[prp + 0x68:prp + 0x78])
        assert (delta + size1) % 0x10000 == 0, "convert: payload end is not on a 64 KiB view boundary"
    if bid is None:
        import hashlib
        h = hashlib.sha256()
        for i in range(0, size1, 1 << 26):
            h.update(payload[i:i + (1 << 26)])
        bid = h.digest()[:16]
    ips, ports = patch_winmm.current_target(hdr + bytes(mm[0x1000:0x60000]))
    port = a.port or ports.pop()
    print(f"finished DLL: SizeOfImage {soi_old:#x}, payload {size1} bytes at file offset {fo:#x} (delta {delta:#x}), fnv {fnv:016x}, "
          f"part1 {size2} bytes, build id {bid.hex()}, target {ips} port {port}")
    # the new image: base + stub + build_dll's chain + the data-file cave
    base = open(a.base, "rb").read()
    out = t2r_pack.write_dll_stub(base, payload[:t2r_pack.STUB_PAYLOAD])
    out = winmm_patches.apply(out, chain_patch_list())
    out = ext_loader.add_loader(out)
    if os.environ.get("T2R_SLOTS8", "1") != "0":
        import slots8
        out, _ = slots8.apply(out)
    out, _, _ = stubpool.add(out)
    stubpool.check(out)
    out = pack_file.add(out, size1, fnv, size2, bid, delta, pack_file.sampled_fnv(payload))
    out = patch_winmm.patch(out, "127.0.0.1", port)
    out = shrink_rsrc.shrink(out)
    pack_file.check(out, size1, fnv, bid)
    secs_new, soi_new = sections(out)
    code_diff_check(mm, secs, out, secs_new)
    os.makedirs(os.path.join(a.out, "t2r-maps", "main"), exist_ok=True)
    dll_out = os.path.join(a.out, "winmm.dll")
    with open(dll_out + ".tmp", "wb") as g:
        g.write(out)
    os.replace(dll_out + ".tmp", dll_out)
    dp = os.path.join(a.out, *pack_file.DATA_REL.split("/"))
    n = pack_file.write_data_file(dp, payload, fnv, bid, delta)
    del payload
    mm.close()
    f.close()
    print(f"{dll_out}: {len(out)} bytes, SizeOfImage {soi_new:#x} (was {soi_old:#x})")
    print(f"{dp}: {n} bytes")


if __name__ == "__main__":
    main()
