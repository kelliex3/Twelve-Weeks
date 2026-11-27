"""Daemon log filtering and rotation discovery."""

from __future__ import annotations

import glob
import os

from store_common import LOG_BYTES, LOG_NOISE, LOG_WARN, logindex, redact


class LogsMixin:
    """Filters daemon logs and discovers rotated files."""

    # ---------------------------------------------------------------- logs
    def cur_log(self):
        """The followed daemon.log when --daemon-log is auto[:dir], else the fixed path."""
        if self.log and self.log.follow is not None:
            return self.log.path or ""
        return self.daemon_log or ""

    def logs(self, q="", level="all", which="0", n=300, quiet=True):
        if self.log:
            self.log.refresh()
        path = self.cur_log()
        if not path:
            return {"ok": False, "error": "no --daemon-log configured"}
        if which and which != "0":
            if not which.isdigit() or int(which) > 9:
                return {"ok": False, "error": "bad log file"}
            path = "%s.%s" % (path, which)
        try:
            size = os.path.getsize(path)
            with open(path, "rb") as fh:
                fh.seek(max(0, size - LOG_BYTES))
                data = fh.read()
        except OSError as exc:
            return {"ok": False, "error": "cannot read %s: %s" % (os.path.basename(path), exc)}

        lines = data.decode("utf-8", "replace").splitlines()
        if size > LOG_BYTES and lines:
            lines = lines[1:]     # a partial first line after the seek
        query = (q or "").lower()
        out = []
        for line in lines:
            if quiet and LOG_NOISE.match(line) and not logindex.ERR_RE.search(line):
                continue
            kind = self._log_kind(line)
            if level == "err" and kind != "err":
                continue
            if level == "warn" and kind not in ("err", "warn"):
                continue
            if level == "evt" and kind != "evt" and " EVT " not in line[:14]:
                continue
            line = redact(line)
            if query and query not in line.lower():
                continue
            out.append({"t": line[:420], "c": kind})
        base = self.cur_log()
        rotated = [p[len(base) + 1:] for p in glob.glob(base + ".*")]
        files = ["0"] + [s for s in sorted(rotated, key=lambda x: (not x.isdigit(), x)) if s.isdigit()]
        return {"ok": True, "lines": out[-n:], "matched": len(out), "file": os.path.basename(path),
                "files": files, "size": size, "mtime": os.path.getmtime(path)}

    @staticmethod
    def _log_kind(line):
        if logindex.ERR_RE.search(line) and not line.startswith(("TCP (", "UDP (")):
            return "err"
        if LOG_WARN.search(line):
            return "warn"
        return "evt" if " EVT " in line[:14] else ""
