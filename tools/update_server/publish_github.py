#!/usr/bin/env python3
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import publish

ASSET_MAX = (2 << 30) - 1
NAME_RE = re.compile(r"[A-Za-z0-9._+-]{1,40}")
ASSET_NAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,120}")
GH = [sys.executable, os.environ["T2R_GH_PY"]] if os.environ.get("T2R_GH_PY") else ["gh"]


def gh(args, dry, capture=False):
    cmd = GH + args
    if dry:
        print("dry run: " + " ".join(cmd))
        return ""
    r = subprocess.run(cmd, check=True, capture_output=capture, text=True)
    return r.stdout if capture else ""


def collect(folder, work):
    os.makedirs(work, exist_ok=True)
    files, assets = [], {}
    for root, dirs, names in os.walk(folder):
        dirs.sort()
        for n in sorted(names):
            if n in publish.SKIP_NAMES:
                continue
            full = os.path.join(root, n)
            rel = os.path.relpath(full, folder).replace(os.sep, "/")
            sha = publish.sha256_file(full)
            size = os.path.getsize(full)
            ent = {"path": rel, "size": size, "sha256": sha}
            assets.setdefault(sha, full)
            gz = os.path.join(work, sha + ".gz")
            if size and not os.path.exists(gz) and not os.path.exists(gz + ".nz"):
                zs = publish._compress_to(full, gz + ".tmp", "gzip")
                if zs <= size * (1 - publish.MIN_SAVING):
                    os.replace(gz + ".tmp", gz)
                else:
                    os.remove(gz + ".tmp")
                    open(gz + ".nz", "wb").close()
            if os.path.exists(gz):
                ent["zsize"], ent["enc"] = os.path.getsize(gz), "gzip"
                assets.setdefault(sha + ".gz", gz)
            files.append(ent)
    if not files:
        raise SystemExit("no files in %s" % folder)
    too_big = [(n, os.path.getsize(p)) for n, p in assets.items() if os.path.getsize(p) > ASSET_MAX]
    if too_big:
        raise SystemExit("GitHub release assets must be under 2 GiB: %s" % ", ".join("%s (%d bytes)" % t for t in too_big))
    return files, assets


def manifest_for(files, version, channel, repo, tag):
    base = "https://github.com/%s/releases/download/%s/" % (repo, tag)
    out = []
    for f in files:
        e = dict(f, url=base + f["sha256"])
        if f.get("enc") == "gzip":
            e["zurl"] = base + f["sha256"] + ".gz"
        out.append(e)
    return {"version": version, "channel": channel, "files": out, "release": {"repo": repo, "tag": tag}}


def check_uploaded(repo, tag, assets, dry):
    if dry:
        return
    got = json.loads(gh(["release", "view", tag, "--repo", repo, "--json", "assets"], False, capture=True))
    have = {a["name"]: a["size"] for a in got.get("assets", [])}
    bad = [n for n, p in assets.items() if have.get(n) != os.path.getsize(p)]
    if bad:
        raise SystemExit("uploaded assets missing or with the wrong size: %s (the release stays a draft)" % ", ".join(bad[:10]))


def release(repo, tag, title, notes, assets, work, dry):
    upload_dir = os.path.join(work, "upload-" + tag)
    shutil.rmtree(upload_dir, ignore_errors=True)
    os.makedirs(upload_dir)
    for name, p in assets.items():
        dst = os.path.join(upload_dir, name)
        try:
            os.link(p, dst)
        except OSError:
            shutil.copyfile(p, dst)
    total = sum(os.path.getsize(p) for p in assets.values())
    print("release %s on %s: %d assets, %.1f MB to upload" % (tag, repo, len(assets), total / 1e6))
    gh(["release", "create", tag, "--repo", repo, "--draft", "--title", title, "--notes", notes], dry)
    names = sorted(os.listdir(upload_dir))
    for i in range(0, len(names), 20):
        gh(["release", "upload", tag, "--repo", repo, "--clobber"] + [os.path.join(upload_dir, n) for n in names[i:i + 20]], dry)
    check_uploaded(repo, tag, assets, dry)
    gh(["release", "edit", tag, "--repo", repo, "--draft=false"], dry)
    shutil.rmtree(upload_dir, ignore_errors=True)


PATCH_KEEP = 0.5     # a patch is listed only when it is under half the file (else the launcher's normal download is as good)


def add_patch(files, assets, patch_path):
    """list the T2PT patch for the file entry it leads to (header "to" = the entry's sha256); the asset is named by the patch's sha256, so
    the launcher finds it next to the file (release/launcher lib/update/files.js: the file url with the patch sha in place of the file sha)"""
    import make_patch
    h = make_patch.read_header(patch_path)
    psha, psize = publish.sha256_file(patch_path), os.path.getsize(patch_path)
    hit = 0
    for f in files:
        if f["sha256"] != h["to"] or psize >= f["size"] * PATCH_KEEP:
            continue
        ps = f.setdefault("patches", [])
        if not any(p["from"] == h["from"] for p in ps):
            ps.append({"from": h["from"], "sha256": psha, "size": psize})
            hit += 1
    if hit:
        assets.setdefault(psha, patch_path)
    return hit


def make_patches(files, assets, bases, work):
    """patch-sized updates: for every file that changed against an older client folder (a build players have installed), a content-defined
    binary patch (make_patch.py file). A 2 MB change in a 700 MB file is then about a 2 MB download. The slim data file is made of
    independent xz groups (tools/slim-rs make.rs pack_groups), so unchanged groups patch as copies."""
    import make_patch
    pdir = os.path.join(work, "patches")
    os.makedirs(pdir, exist_ok=True)
    for base in bases or []:
        for f in list(files):
            old = os.path.join(base, *f["path"].split("/"))
            if not f["size"] or not os.path.isfile(old):
                continue
            osha = publish.sha256_file(old)
            if osha == f["sha256"] or any(p["from"] == osha for p in f.get("patches", [])):
                continue
            out = os.path.join(pdir, "%s-%s.t2pt" % (osha[:12], f["sha256"][:12]))
            if not os.path.exists(out):
                make_patch.make("file", [old, assets[f["sha256"]]], out + ".tmp")
                os.replace(out + ".tmp", out)
            n = add_patch(files, assets, out)
            print("patch %s: %s -> %s %.2f MB%s" % (f["path"], osha[:8], f["sha256"][:8], os.path.getsize(out) / 1e6, "" if n else " (not listed: too big)"))


def publish_client(a, work):
    tag = a.tag or "build-%s" % a.version
    files, assets = collect(a.folder, work)
    make_patches(files, assets, a.patch_base, work)
    for p in a.patch or []:
        if not add_patch(files, assets, p):
            print("patch %s: no file of this build is its target (or it is too big) - skipped" % p)
    m = manifest_for(files, a.version, a.channel, a.repo, tag)
    mpath = os.path.join(work, "manifest.json")
    with open(mpath, "w", encoding="utf-8") as f:
        json.dump(m, f, indent=1)
    assets["manifest.json"] = mpath
    release(a.repo, tag, "Twelve Weeks build %s" % a.version,
            "Client files for the launcher (build %s, channel %s). The launcher downloads these itself; there is nothing to install by hand." % (a.version, a.channel),
            assets, work, a.dry_run)
    if a.data and not a.dry_run:
        publish.write_json(os.path.join(a.data, "manifests", a.channel, a.version + ".json"), m)
        if not a.no_promote:
            publish.promote_version(a.data, a.channel, a.version)
        print("manifest %s/%s written to %s%s" % (a.channel, a.version, a.data, "" if a.no_promote else " and promoted"))
    print("manifest: %s" % mpath)


def publish_launcher(a, work):
    tag = a.launcher_tag or "launcher-%s" % a.launcher_version
    base = "https://github.com/%s/releases/download/%s/" % (a.repo, tag)
    picks = [(p, j) for p, j in ((a.launcher, "launcher.json"), (a.launcher_linux, "launcher-linux.json")) if p]
    assets, infos = {}, {}
    for p, j in picks:
        name = os.path.basename(p)
        if not ASSET_NAME_RE.fullmatch(name):
            raise SystemExit("%s: launcher file names may only use letters, digits and ._+- (GitHub renames anything else)" % name)
        if name in assets:
            raise SystemExit("the Windows and Linux launcher files need different names")
        if os.path.getsize(p) > ASSET_MAX:
            raise SystemExit("%s is over the 2 GiB GitHub asset limit" % name)
        assets[name] = p
        infos[j] = {"version": a.launcher_version, "sha256": publish.sha256_file(p), "size": os.path.getsize(p), "file": name, "url": base + name}
    release(a.repo, tag, "Twelve Weeks Launcher %s" % a.launcher_version,
            "Launcher %s. Download the Windows installer (.exe) or the Linux AppImage below; an installed launcher updates itself." % a.launcher_version,
            assets, work, a.dry_run)
    for j, info in infos.items():
        print("%s: %s" % (j, info["url"]))
        if a.data and not a.dry_run:
            publish.write_json(os.path.join(a.data, j), info)
    if a.data and not a.dry_run:
        print("launcher %s written to %s" % (a.launcher_version, a.data))


def main():
    ap = argparse.ArgumentParser(description="publish a client build and/or the launcher as GitHub releases; the update server's manifest then points the launcher there")
    ap.add_argument("folder", nargs="?", help="the built client folder")
    ap.add_argument("--repo", required=True, help="owner/name of the GitHub repository that holds the releases")
    ap.add_argument("--version", help="client build version (with a folder)")
    ap.add_argument("--channel", default="stable")
    ap.add_argument("--tag", help="client release tag (default build-<version>)")
    ap.add_argument("--launcher", help="Windows launcher installer (.exe)")
    ap.add_argument("--launcher-linux", help="Linux launcher AppImage")
    ap.add_argument("--launcher-version", help="launcher version (with --launcher / --launcher-linux)")
    ap.add_argument("--launcher-tag", help="launcher release tag (default launcher-<version>)")
    ap.add_argument("--data", help="update server data folder: the manifest / launcher.json files are written there")
    ap.add_argument("--no-promote", action="store_true")
    ap.add_argument("--work", help="folder for the compressed copies (default: a temp folder)")
    ap.add_argument("--patch-base", action="append", help="an older client folder (repeatable): changed files get a patch from it")
    ap.add_argument("--patch", action="append", help="a ready T2PT patch file to list (repeatable)")
    ap.add_argument("--dry-run", action="store_true", help="print the gh commands and write the manifest next to the work files only")
    a = ap.parse_args()
    has_launcher = bool(a.launcher or a.launcher_linux)
    if not a.folder and not has_launcher:
        raise SystemExit("nothing to publish: give a client folder and/or --launcher / --launcher-linux")
    if a.folder and (not a.version or not NAME_RE.fullmatch(a.version) or not NAME_RE.fullmatch(a.channel)):
        raise SystemExit("--version / --channel: letters, digits and ._+- only (a client folder needs --version)")
    if has_launcher and (not a.launcher_version or not NAME_RE.fullmatch(a.launcher_version)):
        raise SystemExit("--launcher-version: letters, digits and ._+- only (needed with --launcher / --launcher-linux)")
    work = a.work or tempfile.mkdtemp(prefix="t2r-gh-")
    if a.folder:
        publish_client(a, work)
    if has_launcher:
        publish_launcher(a, work)


if __name__ == "__main__":
    main()
