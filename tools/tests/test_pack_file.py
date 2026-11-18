"""The winmm data-file cave (tools/features/pack_file.py) EXECUTED in a fake image by tools/tests/pack_cave_harness.c: data file present /
missing / other build / truncated / damaged payload, Part I present / absent / other build, no Part I data. Plus end to end on the real base
DLL: t2r_pack.pack_file_bytes -> pack_file.add -> shrink_rsrc; SizeOfImage, the loader's stub check, and the cave's views decode to the
packed files.
Run: python tools/tests/test_pack_file.py   (needs the msys64 gcc; skipped without it)"""
import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "features"))
sys.path.insert(0, os.path.join(HERE, ".."))
import pack_file as M  # noqa: E402

GCC = next((c for c in ("E:/ClaudeTLOU/TLOU2MP Revival/tools/win/ext/msys64/ucrt64/bin/gcc.exe",
                        os.path.join(HERE, "..", "win", "ext", "msys64", "ucrt64", "bin", "gcc.exe")) if os.path.isfile(c)), None)
BASE_DLL = next((c for c in ("E:/ClaudeTLOU/TLOU2MP Revival/tools/local/winmm-nocave-127.0.0.1-7001.dll",
                             os.path.join(HERE, "..", "local", "winmm-nocave-127.0.0.1-7001.dll")) if os.path.isfile(c)), None)
SEC_VA = 0x200000
SIZE1 = 5 * 0x10000                    # a Part I build cuts on a 64 KiB boundary
SIZE2 = 150000
BID = bytes(range(16))


def fnv(b):
    import t2r_pack
    return t2r_pack.fnv_payload_py(b)


@unittest.skipUnless(BASE_DLL, "base winmm dll not found")
class BaseDll(unittest.TestCase):
    def test_iat_and_hook(self):
        import pefile
        pe = pefile.PE(BASE_DLL, fast_load=True)
        pe.parse_data_directories([pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
        have = {}
        for e in pe.DIRECTORY_ENTRY_IMPORT:
            for i in e.imports:
                if i.name:
                    have[i.name.decode()] = i.address
        for n, va in M.IAT.items():
            self.assertEqual(have.get(n), va, n)
        d = open(BASE_DLL, "rb").read()
        off = pe.get_offset_from_rva(M.HOOK_VMA - M.IMAGE_BASE)
        self.assertEqual(d[off:off + 6], M.HOOK_ORIG)
        out = M.add(d, SIZE1, 1, SIZE2, BID, sfnv=1)
        self.assertGreater(M.check(out, SIZE1, 1, BID), 0)
        with self.assertRaises(SystemExit):
            M.add(out, SIZE1, 1, SIZE2, BID, sfnv=1)          # twice: refused


@unittest.skipUnless(GCC, "gcc (msys64 ucrt64) not found")
class Cave(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.td = tempfile.TemporaryDirectory()
        cls.exe = os.path.join(cls.td.name, "h.exe")
        cls.env = dict(os.environ, PATH=os.path.dirname(GCC) + os.pathsep + os.environ["PATH"])
        r = subprocess.run([GCC, "-std=gnu2x", "-O1", "-Wall", "-Wno-cast-function-type", "-o", cls.exe, os.path.join(HERE, "pack_cave_harness.c")],
                           capture_output=True, text=True, env=cls.env)
        assert r.returncode == 0, r.stderr

    @classmethod
    def tearDownClass(cls):
        cls.td.cleanup()

    def scenario(self, size1=SIZE1, size2=SIZE2, data=True, data_id=BID, data_cut=0, data_flip=False, side=True, side_size=SIZE2,
                 side_id=BID, hdr_size=None, tail=0, delta=0, hdr_delta=None):
        d = tempfile.mkdtemp(dir=self.td.name)
        payload = os.urandom(size1 - tail) + b"\x01\x02\x03"[:tail]
        h = fnv(payload)
        os.makedirs(os.path.join(d, "t2r-maps", "main"))
        if data:
            body = bytearray(M.data_header(hdr_size if hdr_size is not None else size1, h, data_id, delta if hdr_delta is None else hdr_delta)
                             + bytes(delta) + payload + data_id)
            if data_flip:
                body[M.HDR + delta + size1 // 2] ^= 0x40
            if data_cut:
                body = body[:-data_cut]
            with open(os.path.join(d, "t2r-maps", "main", "t2r-data.bin"), "wb") as f:
                f.write(body)
        os.makedirs(os.path.join(d, "t2r-maps", "part1"))
        if side:
            with open(os.path.join(d, "t2r-maps", "part1", "part1.dat"), "wb") as f:
                f.write(os.urandom(side_size) + side_id)
        sec, entry, _ = M.build_section(SEC_VA, size1, h, size2, BID, delta, M.sampled_fnv(payload))
        sb = os.path.join(d, "sec.bin")
        with open(sb, "wb") as f:
            f.write(sec)
        self.last = (sb, entry, d, size1, size2)
        return self.rerun()

    def rerun(self):
        """the same image + folder again (the next boot)"""
        sb, entry, d, size1, size2 = self.last
        r = subprocess.run([self.exe, sb, "%x" % (M.IMAGE_BASE + SEC_VA), "%x" % entry, d, str(size1), str(size2)],
                           capture_output=True, text=True, env=self.env, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        out = r.stdout
        kv = dict(x.split("=") for x in out.split("RESULT ")[1].split("\n")[0].split())
        return kv, out

    def data_path(self, name="t2r-data.bin"):
        return os.path.join(self.last[2], "t2r-maps", "main", name)

    def flip(self, off, keep_time):
        p = self.data_path()
        st = os.stat(p)
        with open(p, "r+b") as f:
            f.seek(off)
            b = f.read(1)
            f.seek(off)
            f.write(bytes([b[0] ^ 0x40]))
        if keep_time:
            os.utime(p, ns=(st.st_atime_ns, st.st_mtime_ns))
        else:
            os.utime(p, ns=(st.st_atime_ns, st.st_mtime_ns + 10_000_000_000))

    def fatal(self, out, msg, code):
        self.assertIn("FATAL Twelve Weeks - code " + code, out)
        self.assertIn(msg.split("\n")[0], out)
        self.assertIn("TERMINATE handle=-1 code=7e", out)
        self.assertNotIn("ret=1", out)

    def test_full(self):
        kv, out = self.scenario()
        self.assertEqual((kv["status"], kv["layout"], kv["mstatus"], kv["ret"], kv["d8"]), ("1", "3", "1", "1", str(SIZE1 + SIZE2)), out)
        self.assertIn("MEM full-ok", out)
        self.assertNotIn("FATAL", out)

    def test_no_part1_baked(self):
        kv, out = self.scenario(size1=SIZE1 + 13, size2=0, side=False, tail=3)        # any size, tail bytes hashed too
        self.assertEqual((kv["status"], kv["layout"], kv["mstatus"], kv["ret"], kv["d8"]), ("0", "3", "1", "1", str(SIZE1 + 13)), out)
        self.assertIn("MEM view1-ok", out)

    def test_part1_absent(self):
        kv, out = self.scenario(side=False)
        self.assertEqual((kv["status"], kv["layout"], kv["mstatus"], kv["ret"], kv["d8"]), ("2", "1", "1", "1", str(SIZE1 + SIZE2)), out)
        self.assertIn("MEM view1-ok-hole-reserved", out)

    def test_part1_other_build(self):
        kv, out = self.scenario(side_id=bytes(16))
        self.assertEqual((kv["status"], kv["layout"], kv["step"], kv["err"], kv["mstatus"], kv["ret"]), ("3", "1", "7", "ffff0004", "1", "1"), out)
        self.assertIn("MEM view1-ok-hole-reserved", out)

    def test_part1_wrong_size(self):
        kv, out = self.scenario(side_size=SIZE2 - 1)
        self.assertEqual((kv["status"], kv["layout"], kv["step"], kv["err"], kv["ret"]), ("3", "1", "6", "ffff0003", "1"), out)
        self.assertIn("MEM view1-ok-hole-reserved", out)

    def test_data_missing(self):
        kv, out = self.scenario(data=False)                        # t2r-maps\main exists: ERROR_FILE_NOT_FOUND
        self.fatal(out, M.MSG_MISSING, "03-00000002")

    def test_data_missing_folder_too(self):
        d = tempfile.mkdtemp(dir=self.td.name)                     # no t2r-maps at all: ERROR_PATH_NOT_FOUND = the same message
        sec, entry, _ = M.build_section(SEC_VA, SIZE1, 5, 0, BID, sfnv=5)
        sb = os.path.join(d, "sec.bin")
        open(sb, "wb").write(sec)
        r = subprocess.run([self.exe, sb, "%x" % (M.IMAGE_BASE + SEC_VA), "%x" % entry, d, str(SIZE1), "0"], capture_output=True, text=True, env=self.env, timeout=60)
        self.fatal(r.stdout, M.MSG_MISSING, "03-00000003")

    def test_data_other_build(self):
        kv, out = self.scenario(data_id=bytes(16))
        self.fatal(out, M.MSG_STALE, "07-FFFF0004")

    def test_data_other_size(self):
        kv, out = self.scenario(data_cut=1)
        self.fatal(out, M.MSG_STALE, "06-FFFF0003")

    def test_data_header_size_wrong(self):
        kv, out = self.scenario(hdr_size=SIZE1 - 8)
        self.fatal(out, M.MSG_STALE, "07-FFFF0004")

    def test_data_damaged(self):
        kv, out = self.scenario(data_flip=True)
        self.fatal(out, M.MSG_BAD, "0A-FFFF0005")

    def test_delta_full(self):                                     # a payload that starts 0x1230 bytes into its view (converted P1-side builds)
        kv, out = self.scenario(size1=SIZE1 - 0x1230, delta=0x1230)
        self.assertEqual((kv["status"], kv["layout"], kv["mstatus"], kv["ret"], kv["d8"]), ("1", "3", "1", "1", str(SIZE1 - 0x1230 + SIZE2)), out)
        self.assertIn("MEM full-ok", out)

    def test_delta_part1_absent(self):
        kv, out = self.scenario(size1=SIZE1 - 0x1230, delta=0x1230, side=False)
        self.assertEqual((kv["status"], kv["layout"], kv["mstatus"], kv["ret"]), ("2", "1", "1", "1"), out)
        self.assertIn("MEM view1-ok-hole-reserved", out)

    def test_delta_header_wrong(self):
        kv, out = self.scenario(size1=SIZE1 - 0x1230, delta=0x1230, hdr_delta=0)
        self.fatal(out, M.MSG_STALE, "07-FFFF0004")

    def test_cache_written_then_hit(self):
        kv, out = self.scenario()
        self.assertEqual((kv["mstatus"], kv["cache"], kv["ret"]), ("1", "2", "1"), out)      # first boot: full check, record written
        rec = open(self.data_path("t2r-data.ok"), "rb").read()
        self.assertEqual(len(rec), 0x30)
        self.assertEqual(rec[:8] + rec[8:24], struct.pack("<II", 0x4B4F3254, 1) + BID)
        self.assertEqual(struct.unpack_from("<Q", rec, 24)[0], os.path.getsize(self.data_path()))
        kv, out = self.rerun()
        self.assertEqual((kv["mstatus"], kv["cache"], kv["ret"]), ("1", "1", "1"), out)      # next boot: record matches, no full check
        self.assertIn("MEM full-ok", out)

    def test_cache_changed_file_full_check(self):                  # a rewritten file (new write time): full check again, damage found
        self.scenario()
        self.flip(M.HDR + SIZE1 // 2, keep_time=False)             # not in a sample
        kv, out = self.rerun()
        self.fatal(out, M.MSG_BAD, "0A-FFFF0005")

    def test_cache_sample_every_boot(self):                        # damage in a sample: found even while the record matches
        self.scenario()
        self.flip(M.HDR + 100, keep_time=True)
        kv, out = self.rerun()
        self.fatal(out, M.MSG_BAD, "0A-FFFF0006")
        self.scenario()
        self.flip(M.HDR + SIZE1 - 5, keep_time=True)               # the last 4 KiB
        kv, out = self.rerun()
        self.fatal(out, M.MSG_BAD, "0A-FFFF0006")

    def test_cache_other_build_record(self):                       # a record from another build: full check, record rewritten
        self.scenario()
        p = self.data_path("t2r-data.ok")
        rec = bytearray(open(p, "rb").read())
        rec[8] ^= 1
        open(p, "wb").write(rec)
        kv, out = self.rerun()
        self.assertEqual((kv["mstatus"], kv["cache"]), ("1", "2"), out)
        self.assertEqual(open(p, "rb").read()[8:24], BID)

    def test_cache_bad_record(self):                               # short / garbage record: full check
        self.scenario()
        open(self.data_path("t2r-data.ok"), "wb").write(b"T2OK")
        kv, out = self.rerun()
        self.assertEqual((kv["mstatus"], kv["cache"]), ("1", "2"), out)

    def test_sampled_fnv_python_matches_samples(self):
        import random
        b = bytearray(os.urandom(0x4000000 + 9000))
        h = M.sampled_fnv(b)
        b[5000] ^= 1                                               # outside every sample: same value
        self.assertEqual(M.sampled_fnv(b), h)
        b[0x4000000 + 10] ^= 1                                     # the 64 MiB sample
        self.assertNotEqual(M.sampled_fnv(b), h)
        del random

    def test_geometry(self):
        kv, out = self.scenario(size1=SIZE1 + 8)                   # the build guarantees the 64 KiB cut; if not, refuse clearly
        self.fatal(out, M.MSG_BAD, "09-FFFF0002")


@unittest.skipUnless(GCC and BASE_DLL, "gcc and the base winmm dll needed")
class EndToEnd(unittest.TestCase):
    """pack_file_bytes on the real base DLL + a small stage, then pack_file.add + shrink_rsrc (the build's last steps), the files fed to the
    cave harness: small SizeOfImage, a stub the loader accepts, and the engine's view decodes to exactly the packed files"""

    def run_case(self, side_paths):
        import pefile
        import t2r_pack as T
        import shrink_rsrc
        td = tempfile.mkdtemp()
        stage = os.path.join(td, "stage")
        files = {}
        for i in range(5):
            files[f"pak68/m{i}.pak"] = os.urandom(20000 + i * 7000)
        files["pak68/p1-a.pak"] = os.urandom(400000)
        files["soundbank4/p1-b.bnk"] = os.urandom(200000)
        for p, d in files.items():
            fp = os.path.join(stage, *p.split("/"))
            os.makedirs(os.path.dirname(fp), exist_ok=True)
            with open(fp, "wb") as f:
                f.write(d)
        paths = sorted(files)
        with open(os.path.join(stage, "_t2r_manifest.txt"), "w", newline="\n") as f:
            f.write("flags=12 trail=0\n" + "\n".join(paths) + "\n--order--\n" + "\n".join(paths) + "\n")
        base = open(BASE_DLL, "rb").read()
        out, payload, side, info = T.pack_file_bytes(base, stage, side_paths)
        out = M.add(out, info["size1"], info["fnv"], info["size2"], info["build_id"], sfnv=info["sfnv"])
        out = shrink_rsrc.shrink(out)
        M.check(out, info["size1"], info["fnv"], info["build_id"])
        pe = pefile.PE(data=out, fast_load=True)
        self.assertLess(pe.OPTIONAL_HEADER.SizeOfImage, 0x0A000000)
        self.assertNotIn(b".t2rc", [s.Name.rstrip(b"\0") for s in pe.sections])
        self.assertLess(len(out), 4 << 20)
        enc, h = T.read_t2me(out)                                  # what the loader checks before the cave
        self.assertEqual((enc, h), (payload[:T.STUB_PAYLOAD], T.fnv_payload(payload[:T.STUB_PAYLOAD])))
        d = os.path.join(td, "run")
        os.makedirs(os.path.join(d, "t2r-maps", "part1"))
        os.makedirs(os.path.join(d, "t2r-maps", "main"))
        n = M.write_data_file(os.path.join(d, "t2r-maps", "main", "t2r-data.bin"), payload, info["fnv"], info["build_id"])
        self.assertEqual(n, M.HDR + info["size1"] + 16)
        pv, hdr = M.read_data_file(os.path.join(d, "t2r-maps", "main", "t2r-data.bin"))
        self.assertEqual((bytes(pv), hdr["fnv"], hdr["build_id"]), (bytes(payload), info["fnv"], info["build_id"]))
        if side:
            self.assertEqual(info["size1"] % 0x10000, 0)
            with open(os.path.join(d, "t2r-maps", "part1", "part1.dat"), "wb") as f:
                f.write(side)
        sva = M.check(out)
        so = next(s for s in pe.sections if s.Name.rstrip(b"\0") == M.NAME)
        sec = out[so.PointerToRawData:so.PointerToRawData + so.SizeOfRawData]
        entry = struct.unpack_from("<i", out, pe.get_offset_from_rva(M.HOOK_VMA - M.IMAGE_BASE) + 1)[0] + M.HOOK_VMA + 5
        with open(os.path.join(td, "sec.bin"), "wb") as f:
            f.write(sec)
        h = os.path.join(td, "h.exe")
        env = dict(os.environ, PATH=os.path.dirname(GCC) + os.pathsep + os.environ["PATH"])
        r = subprocess.run([GCC, "-std=gnu2x", "-O1", "-o", h, os.path.join(HERE, "pack_cave_harness.c")], capture_output=True, text=True, env=env)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = subprocess.run([h, os.path.join(td, "sec.bin"), "%x" % (M.IMAGE_BASE + sva), "%x" % entry, d, str(info["size1"]), str(info["size2"])],
                           capture_output=True, text=True, env=env, timeout=120)
        self.assertIn("mstatus=1", r.stdout, r.stdout + r.stderr)
        self.assertIn("ret=1", r.stdout)
        self.assertIn("MEM full-ok" if side else "MEM view1-ok", r.stdout)
        whole = bytes(payload) + (bytes(side[:-16]) if side else b"")
        m, f2 = T.psarc_read(T.dsar_read(T.keystream_xor(whole)))
        self.assertEqual((f2, m["paths"]), (files, paths))

    def test_with_part1(self):
        self.run_case(["pak68/p1-a.pak", "soundbank4/p1-b.bnk"])

    def test_without_part1(self):
        self.run_case(())


if __name__ == "__main__":
    unittest.main(verbosity=2)
