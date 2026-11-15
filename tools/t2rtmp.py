"""Temp-file hygiene for every tool and test (%TEMP% grew to 95 GB, mostly leaked tempfile.mkdtemp() dirs of tests).

`import t2rtmp` (once, before the first tempfile call) does four things:
  * T2R_TMP=<dir>      all temp files of this process (and of its children: TMP/TEMP/TMPDIR) go there instead of %TEMP%
                       (e.g. a folder on another drive when C: is short on space)
  * a named prefix     tempfile.mkdtemp()/mkstemp()/NamedTemporaryFile(delete=False) without an explicit prefix get
                       't2r-tmp-<script>-', so leftovers are identifiable and safe to delete (`t2r-tmp-*`)
  * cleanup at exit    every dir / file made through those calls is removed when the process ends (atexit; success, failure
                       or an exception). T2R_KEEP_TMP=1 keeps them all; t2rtmp.keep(path) keeps one (e.g. a preview PNG the
                       caller is about to open after the script ends)
  * a stale sweep      at import, `t2r-tmp-*` entries of the temp dir older than T2R_TMP_SWEEP_HOURS (default 6) are removed:
                       what a killed process (no atexit) left behind. Only our own prefix is ever touched.
Standalone: `python tools/t2rtmp.py sweep [hours]` runs just the sweep, `python tools/t2rtmp.py list` shows the leftovers."""
import atexit
import os
import shutil
import stat
import sys
import tempfile
import time

PREFIX = "t2r-tmp-"
_owned = []          # paths to remove at exit (dirs and files)
_installed = False


def _script():
    n = os.path.splitext(os.path.basename(sys.argv[0] or "py"))[0] if sys.argv else "py"
    n = "".join(c if c.isalnum() or c in "_-" else "_" for c in n)[:24]
    return n or "py"


def _rm(path):
    def onerr(fn, p, exc):                 # read-only files (git objects, extracted game files) block rmtree on Windows
        try:
            os.chmod(p, stat.S_IWRITE)
            fn(p)
        except OSError:
            pass
    try:
        if os.path.isdir(path) and not os.path.islink(path):
            shutil.rmtree(path, onerror=onerr)
        elif os.path.lexists(path):
            os.remove(path)
    except OSError:
        pass


def keep(path):
    """Do not remove `path` at exit (it stays until the next stale sweep). -> path"""
    try:
        _owned.remove(path)
    except ValueError:
        pass
    return path


def _own(path):
    if os.environ.get("T2R_KEEP_TMP") != "1":
        _owned.append(path)
    return path


own = _own        # public: t2rtmp.own(path) -> path, removed at exit


def _cleanup():
    for p in reversed(_owned):
        _rm(p)
    del _owned[:]


def sweep(hours=None, base=None, log=None):
    """Remove `t2r-tmp-*` entries older than `hours` (default T2R_TMP_SWEEP_HOURS or 6) from the temp dir. -> [removed names]"""
    if hours is None:
        try:
            hours = float(os.environ.get("T2R_TMP_SWEEP_HOURS", "6"))
        except ValueError:
            hours = 6.0
    base = base or tempfile.gettempdir()
    cut = time.time() - hours * 3600
    gone = []
    try:
        with os.scandir(base) as it:
            for e in it:
                if not e.name.startswith(PREFIX):
                    continue
                try:
                    if e.stat(follow_symlinks=False).st_mtime > cut:
                        continue
                except OSError:
                    continue
                _rm(e.path)
                gone.append(e.name)
    except OSError:
        pass
    if log and gone:
        log("t2rtmp: swept %d stale temp entries" % len(gone))
    return gone


def install():
    global _installed
    if _installed:
        return
    _installed = True
    base = os.environ.get("T2R_TMP")
    if base and tempfile.tempdir is None:
        try:
            os.makedirs(base, exist_ok=True)
            tempfile.tempdir = os.path.abspath(base)
            for k in ("TMP", "TEMP", "TMPDIR"):
                os.environ[k] = tempfile.tempdir
        except OSError:
            pass
    orig_mkdtemp, orig_mkstemp, orig_ntf = tempfile.mkdtemp, tempfile.mkstemp, tempfile.NamedTemporaryFile

    def mkdtemp(suffix=None, prefix=None, dir=None):
        if prefix is None:
            prefix = PREFIX + _script() + "-"
        return _own(orig_mkdtemp(suffix, prefix, dir))

    def mkstemp(suffix=None, prefix=None, dir=None, text=False):
        if prefix is None:
            prefix = PREFIX + _script() + "-"
        fd, path = orig_mkstemp(suffix, prefix, dir, text)
        _own(path)
        return fd, path

    def NamedTemporaryFile(mode="w+b", buffering=-1, encoding=None, newline=None, suffix=None, prefix=None, dir=None, delete=True, **kw):
        if prefix is None and not delete:
            prefix = PREFIX + _script() + "-"
        f = orig_ntf(mode=mode, buffering=buffering, encoding=encoding, newline=newline, suffix=suffix, prefix=prefix, dir=dir, delete=delete, **kw)
        if not delete:
            _own(f.name)
        return f

    tempfile.mkdtemp, tempfile.mkstemp, tempfile.NamedTemporaryFile = mkdtemp, mkstemp, NamedTemporaryFile
    atexit.register(_cleanup)
    sweep()


install()


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "list"
    if cmd == "sweep":
        g = sweep(float(sys.argv[2]) if len(sys.argv) > 2 else None)
        print("removed %d entries" % len(g))
    else:
        n = 0
        for e in os.scandir(tempfile.gettempdir()):
            if e.name.startswith(PREFIX):
                n += 1
                print(e.name, time.strftime("%m-%d %H:%M", time.localtime(e.stat(follow_symlinks=False).st_mtime)))
        print("%d t2r-tmp-* entries in %s" % (n, tempfile.gettempdir()))
