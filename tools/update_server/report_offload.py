#!/usr/bin/env python3
"""Spread bug-report files over the storage boxes, so the report server's own disk does not fill up.

The launcher still uploads every report to the report server (report_store.py). This job (cron, every 10 minutes) moves the bigger
files of each report (logs, dumps, clips) to one of the storage boxes listed in the boxes file, and leaves meta.json and the small
files on the report server. The admin site reads meta.json as before; a moved file is fetched back from its box on demand
(store.py, report_local_path).

Per report folder <reports>/<folder>/:
  files/<name>      still here (small, too new, or not moved yet)
  store.json        {"box": "<name>", "files": {"<name>": <size>, ...}}: files that now live on that box, at <folder>/files/<name>

One report = one box (all its moved files go to the same box). A new report goes to the box with the lowest used / capacity share.
Files are copied with rsync --remove-source-files: rsync checks each copied file and only then deletes the local copy.

The boxes allow this key only through rrsync, rooted at ~/t2r-reports (authorized_keys:
  restrict,command="/usr/bin/rrsync /home/<user>/t2r-reports" ssh-ed25519 ...).

Boxes file (JSON): {"key": "~/.ssh/t2r_reports", "known_hosts": "~/.ssh/known_hosts_reports",
                    "boxes": [{"name": "box-a", "host": "user@box-a.example", "gb": 18}, ...]}
"""
import argparse
import json
import os
import subprocess
import sys
import time

KEEP_SMALL = 64 * 1024        # files up to this size stay on the report server (system.txt, version, settings ...)
MIN_AGE = 15 * 60             # never touch a file younger than this (a report or clip may still be arriving)
RSYNC_TIMEOUT = 120


def load_boxes(path):
    with open(os.path.expanduser(path), encoding="utf-8") as f:
        cfg = json.load(f)
    boxes = [b for b in cfg.get("boxes", []) if b.get("name") and b.get("host")]
    if not boxes:
        raise SystemExit("no boxes in %s" % path)
    return cfg, boxes


def read_store(d):
    try:
        with open(os.path.join(d, "store.json"), encoding="utf-8") as f:
            s = json.load(f)
        if isinstance(s, dict) and isinstance(s.get("files"), dict):
            return s
    except (OSError, ValueError):
        pass
    return None


def write_store(d, s):
    p = os.path.join(d, "store.json")
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(s, f, indent=1)
    os.replace(tmp, p)


def ssh_cmd(cfg):
    return "ssh -i %s -o BatchMode=yes -o StrictHostKeyChecking=yes -o UserKnownHostsFile=%s -o ConnectTimeout=15" % (
        os.path.expanduser(cfg.get("key", "~/.ssh/t2r_reports")), os.path.expanduser(cfg.get("known_hosts", "~/.ssh/known_hosts_reports")))


def candidates(d, now, keep_small=KEEP_SMALL, min_age=MIN_AGE):
    """Names in <d>/files that should move now: regular files, not hidden (.clip-*.part uploads), old enough, bigger than keep_small."""
    fd = os.path.join(d, "files")
    out = []
    try:
        names = sorted(os.listdir(fd))
    except OSError:
        return out
    for n in names:
        if n.startswith(".") or "/" in n:
            continue
        p = os.path.join(fd, n)
        try:
            st = os.lstat(p)
        except OSError:
            continue
        if not os.path.isfile(p) or os.path.islink(p):
            continue
        if now - st.st_mtime < min_age or st.st_size <= keep_small:
            continue
        out.append((n, st.st_size))
    return out


def box_usage(root, boxes):
    used = {b["name"]: 0 for b in boxes}
    for folder in os.listdir(root):
        s = read_store(os.path.join(root, folder))
        if s and s.get("box") in used:
            used[s["box"]] += sum(int(v) for v in s["files"].values())
    return used


def pick_box(boxes, used):
    return min(boxes, key=lambda b: (used.get(b["name"], 0) / (float(b.get("gb", 10)) * (1 << 30)), b["name"]))


def rsync_up(cfg, box, d, folder, names, run=subprocess.run):
    """Copy files/<names> to <box>:<folder>/files/ and delete each local copy rsync confirmed. True on rsync exit 0."""
    lst = "\n".join(names) + "\n"
    cmd = ["rsync", "-t", "--mkpath", "--remove-source-files", "--timeout=%d" % RSYNC_TIMEOUT, "-e", ssh_cmd(cfg),
           "--files-from=-", os.path.join(d, "files") + "/", "%s:%s/files/" % (box["host"], folder)]
    r = run(cmd, input=lst, text=True, capture_output=True, timeout=RSYNC_TIMEOUT * 4)
    if r.returncode != 0:
        sys.stderr.write("rsync to %s for %s: exit %d %s\n" % (box["name"], folder, r.returncode, (r.stderr or "").strip()[-300:]))
    return r.returncode == 0


def offload(root, cfg, boxes, now=None, run=subprocess.run, max_bytes=None, log=print):
    """One pass over all report folders. Returns (files moved, bytes moved)."""
    now = time.time() if now is None else now
    byname = {b["name"]: b for b in boxes}
    used = box_usage(root, boxes)
    moved_n = moved_b = 0
    for folder in sorted(os.listdir(root)):
        d = os.path.join(root, folder)
        if not os.path.isfile(os.path.join(d, "meta.json")):
            continue
        todo = candidates(d, now)
        if not todo:
            continue
        s = read_store(d) or {"files": {}}
        box = byname.get(s.get("box")) if s.get("box") else None
        if s.get("box") and box is None:
            log("skip %s: its box %s is not in the boxes file" % (folder, s.get("box")))
            continue
        if box is None:
            box = pick_box(boxes, used)
            s["box"] = box["name"]
        sizes = dict(todo)
        ok = rsync_up(cfg, box, d, folder, [n for n, _ in todo], run=run)
        gone = [n for n in sizes if not os.path.exists(os.path.join(d, "files", n))]
        if gone:
            for n in gone:
                s["files"][n] = sizes[n]
            write_store(d, s)
            b = sum(sizes[n] for n in gone)
            used[box["name"]] = used.get(box["name"], 0) + b
            moved_n += len(gone)
            moved_b += b
            log("%s -> %s: %d files, %.1f MB%s" % (folder, box["name"], len(gone), b / 1e6, "" if ok else " (rsync failed part way)"))
        if max_bytes is not None and moved_b >= max_bytes:
            break
    return moved_n, moved_b


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--reports", required=True, help="the reports folder (update-data/reports)")
    ap.add_argument("--boxes", required=True, help="boxes JSON file")
    ap.add_argument("--max-gb", type=float, default=None, help="stop a pass after this many GB")
    ap.add_argument("--lock", default="/tmp/t2r-report-offload.lock")
    a = ap.parse_args(argv)
    import fcntl
    cfg, boxes = load_boxes(a.boxes)
    with open(a.lock, "w") as lk:
        try:
            fcntl.flock(lk, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            return 0          # the previous pass is still running
        stamp = time.strftime("%Y-%m-%d %H:%M:%S")
        n, b = offload(a.reports, cfg, boxes, max_bytes=None if a.max_gb is None else int(a.max_gb * (1 << 30)),
                       log=lambda m: print("%s %s" % (stamp, m), flush=True))
        if n:
            print("%s pass done: %d files, %.1f MB moved" % (stamp, n, b / 1e6), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
