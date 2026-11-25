"""report_offload: which files move, box choice, store.json bookkeeping, failure keeps local files. rsync is faked (copies into a local
'box' folder and removes the sources it copied, like --remove-source-files)."""
import json
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import report_offload as ro  # noqa: E402

FAILS = []


def check(name, cond):
    print(("ok   " if cond else "FAIL ") + name)
    if not cond:
        FAILS.append(name)


def mk_report(root, folder, files, age):
    d = os.path.join(root, folder)
    os.makedirs(os.path.join(d, "files"))
    with open(os.path.join(d, "meta.json"), "w") as f:
        json.dump({"code": folder.split("_")[1], "files": [{"name": n, "size": s} for n, s in files.items()]}, f)
    for n, s in files.items():
        p = os.path.join(d, "files", n)
        with open(p, "wb") as f:
            f.write(b"x" * s)
        t = time.time() - age
        os.utime(p, (t, t))
    return d


class FakeRsync:
    def __init__(self, boxdir, fail_after=None):
        self.boxdir, self.fail_after, self.calls = boxdir, fail_after, []

    def __call__(self, cmd, input="", **kw):
        src, dst = cmd[-2], cmd[-1]
        host, rel = dst.split(":", 1)
        names = [n for n in input.splitlines() if n]
        self.calls.append((host, rel, names))
        out = os.path.join(self.boxdir, host, rel)
        os.makedirs(out, exist_ok=True)
        rc = 0
        for i, n in enumerate(names):
            if self.fail_after is not None and i >= self.fail_after:
                rc = 23
                break
            shutil.copy2(os.path.join(src, n), os.path.join(out, n))
            os.remove(os.path.join(src, n))

        class R:
            returncode, stderr = rc, "fake"
        return R()


def main():
    tmp = tempfile.mkdtemp()
    try:
        root, boxdir = os.path.join(tmp, "reports"), os.path.join(tmp, "boxes")
        os.makedirs(root)
        cfg = {"key": "~/k", "known_hosts": "~/kh"}
        boxes = [{"name": "box-a", "host": "u@n", "gb": 20}, {"name": "box-b", "host": "u@s", "gb": 20}, {"name": "box-c", "host": "u@f", "gb": 10}]
        big = 200 * 1024
        a = mk_report(root, "20261125-000001_R-AAAA_p", {"t2r-ext.log": big, "system.txt": 100, "dump.dmp": big * 3}, age=3600)
        b = mk_report(root, "20261125-000002_R-BBBB_p", {"t2r-ext.log": big}, age=60)            # too new
        c = mk_report(root, "20261125-000003_R-CCCC_p", {"t2r-ext.log": big, ".clip-1.part": big}, age=3600)
        fake = FakeRsync(boxdir)
        n, nb = ro.offload(root, cfg, boxes, run=fake, log=lambda m: None)
        check("2 reports moved (A: log+dump, C: log), 3 files", n == 3 and nb == big * 5)
        check("small system.txt stays local", os.path.exists(os.path.join(a, "files", "system.txt")))
        check("too-new report untouched", os.path.exists(os.path.join(b, "files", "t2r-ext.log")) and not os.path.exists(os.path.join(b, "store.json")))
        check("hidden .clip part untouched", os.path.exists(os.path.join(c, "files", ".clip-1.part")))
        sa, sc = ro.read_store(a), ro.read_store(c)
        check("store.json A lists both files", sa and set(sa["files"]) == {"t2r-ext.log", "dump.dmp"})
        check("A and C on different boxes (lowest share)", sa["box"] != sc["box"])
        boxpath = os.path.join(boxdir, dict((x["name"], x["host"]) for x in boxes)[sa["box"]], os.path.basename(a), "files", "dump.dmp")
        check("dump on the box at <folder>/files/<name>", os.path.getsize(boxpath) == big * 3)
        check("meta.json untouched", json.load(open(os.path.join(a, "meta.json")))["code"] == "R-AAAA")
        # a clip arriving later on an offloaded report joins the same box
        p = os.path.join(a, "files", "clip-1.mp4")
        with open(p, "wb") as f:
            f.write(b"y" * big)
        t = time.time() - 3600
        os.utime(p, (t, t))
        ro.offload(root, cfg, boxes, run=fake, log=lambda m: None)
        sa2 = ro.read_store(a)
        check("late clip goes to the same box", sa2["box"] == sa["box"] and "clip-1.mp4" in sa2["files"])
        # rsync failing part way: only the files rsync removed are recorded
        d = mk_report(root, "20261125-000004_R-DDDD_p", {"a.log": big, "b.log": big}, age=3600)
        ro.offload(root, cfg, boxes, run=FakeRsync(boxdir, fail_after=1), log=lambda m: None)
        sd = ro.read_store(d)
        check("partial rsync: 1 recorded, 1 kept local", sd and list(sd["files"]) == ["a.log"] and os.path.exists(os.path.join(d, "files", "b.log")))
        # unknown box in store.json: skip, keep files
        e = mk_report(root, "20261125-000005_R-EEEE_p", {"a.log": big}, age=3600)
        ro.write_store(e, {"box": "gone1", "files": {}})
        ro.offload(root, cfg, boxes, run=fake, log=lambda m: None)
        check("unknown box: files stay", os.path.exists(os.path.join(e, "files", "a.log")))
        # capacity weighting: box-c has half the capacity
        used = {"box-a": 10, "box-b": 10, "box-c": 6}
        check("pick by used/capacity share", ro.pick_box(boxes, used)["name"] in ("box-a", "box-b"))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("%d failure(s)" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
