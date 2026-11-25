"""Admin store: report files moved to a storage box (store.json) are read back through report_local_path (local file, cache, or rsync fetch)."""
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import store  # noqa: E402

FAILS = []


def check(name, cond):
    print(("ok   " if cond else "FAIL ") + name)
    if not cond:
        FAILS.append(name)


def main():
    tmp = tempfile.mkdtemp()
    real_run = subprocess.run
    try:
        reports = os.path.join(tmp, "update-data", "reports")
        folder = "20261125-000001_R-AAAA_p"
        d = os.path.join(reports, folder)
        os.makedirs(os.path.join(d, "files"))
        meta = {"code": "R-AAAA", "files": [{"name": "system.txt", "size": 5}, {"name": "t2r-ext.log", "size": 11},
                                             {"name": "shot.png", "size": 12, "kind": "screenshot"}]}
        json.dump(meta, open(os.path.join(d, "meta.json"), "w"))
        open(os.path.join(d, "files", "system.txt"), "w").write("hello")
        json.dump({"box": "nyc1", "files": {"t2r-ext.log": 11, "shot.png": 12}}, open(os.path.join(d, "store.json"), "w"))
        boxes = os.path.join(tmp, "boxes.json")
        json.dump({"key": "~/k", "known_hosts": "~/kh", "boxes": [{"name": "nyc1", "host": "u@n"}]}, open(boxes, "w"))
        os.environ["T2R_REPORT_BOXES"] = boxes
        os.environ["T2R_REPORT_CACHE"] = os.path.join(tmp, "data", "report-cache")
        A = store.Admin.__new__(store.Admin)
        A.reports = reports
        A.report_metas = lambda: [(d, meta)]
        calls = []
        remote = {"u@n:%s/files/t2r-ext.log" % folder: b"remote-log!", "u@n:%s/files/shot.png" % folder: b"\x89PNG\r\n\x1a\nabcd"}

        def fake_run(cmd, **kw):
            calls.append(cmd)
            src, dst = cmd[-2], cmd[-1]

            class R:
                returncode = 0 if src in remote else 23
            if src in remote:
                open(dst, "wb").write(remote[src])
            return R()
        store.subprocess.run = fake_run
        check("local file read as before", A.report_file("R-AAAA", "system.txt")["text"] == "hello" and not calls)
        r = A.report_file("R-AAAA", "t2r-ext.log")
        check("moved log fetched from its box", r and r["text"] == "remote-log!" and len(calls) == 1)
        cache = os.path.join(tmp, "data", "report-cache", folder, "t2r-ext.log")
        check("cached in T2R_REPORT_CACHE", os.path.isfile(cache))
        A.report_file("R-AAAA", "t2r-ext.log")
        check("second read uses the cache", len(calls) == 1)
        img = A.report_image("R-AAAA", "shot.png")
        check("moved screenshot fetched + sniffed", img and img[0] == "image/png")
        check("unlisted name refused", A.report_file("R-AAAA", "../meta.json") is None)
        check("name with a slash refused", A.report_local_path(d, "../meta.json") is None)
        remote.clear()
        os.remove(cache)
        check("box fetch failing gives None", A.report_file("R-AAAA", "t2r-ext.log") is None and not os.path.exists(cache + ".part"))
        os.environ.pop("T2R_REPORT_BOXES")
        check("no boxes file configured gives None", A.report_local_path(d, "t2r-ext.log") is None)
    finally:
        store.subprocess.run = real_run
        shutil.rmtree(tmp, ignore_errors=True)
    print("%d failure(s)" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
