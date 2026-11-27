"""Bug-report listing and file access."""

from __future__ import annotations

import json
import os
import secrets

from store_common import (
    IMAGE_TYPES, MAX_REPORT_IMAGE, MAX_REPORT_TEXT, REPORT_CODE, REPORT_TEXT_SUFFIXES,
    image_type, video_type,
)


class ReportsMixin:
    """Lists bug reports and serves their attachments."""

    # ---------------------------------------------------------------- bug reports
    def report_metas(self):
        """(folder, meta.json dict) for every saved report, newest folder first."""
        if not self.reports or not os.path.isdir(self.reports):
            return []
        out = []
        for name in sorted(os.listdir(self.reports), reverse=True):
            path = os.path.join(self.reports, name, "meta.json")
            if os.path.isfile(path):
                try:
                    with open(path, encoding="utf-8") as fh:
                        out.append((os.path.join(self.reports, name), json.load(fh)))
                except (OSError, ValueError):
                    pass
        return out

    def reports_list(self):
        rows = []
        for _, meta in self.report_metas():
            text = (meta.get("text") or "").strip()
            rows.append({
                "code": meta.get("code"), "received": meta.get("received"), "name": meta.get("name"),
                "version": meta.get("version"), "mapMode": meta.get("mapMode"),
                "text": text.splitlines()[0][:120] if text else "", "handled": bool(meta.get("handled")),
                "files": len(meta.get("files", [])),
            })
        # Kept for legacy API compatibility; the old site also wrote this token to
        # reports.txt in its working directory, which served no purpose and is dropped.
        return {"reports": rows, "configured": bool(self.reports),
                "new": len([r for r in rows if not r["handled"]]), "uniqueCode": secrets.token_urlsafe(32)}

    def find_report(self, code):
        normalized = (code or "").upper()
        if not REPORT_CODE.match(normalized):
            return None
        for folder, meta in self.report_metas():
            if (meta.get("code") or "").upper() == normalized:
                return folder, meta
        return None

    def report(self, code):
        found = self.find_report(code)
        if not found:
            return {"ok": False, "error": "no such report"}
        _, meta = found
        meta = dict(meta)
        meta.pop("clipKey", None)   # the launcher's upload key is never shown
        files = [dict(f, text=f["name"].lower().endswith(REPORT_TEXT_SUFFIXES),
                      image=f.get("kind") == "screenshot", video=f.get("kind") == "video")
                 for f in meta.get("files", [])]
        return {"ok": True, "meta": meta, "files": files}

    def report_video(self, code, name):
        """(path, content-type, size) of an attached clip, or None."""
        found = self.find_report(code)
        if not found:
            return None
        folder, meta = found
        if not any(f.get("name") == name and f.get("kind") == "video" for f in meta.get("files", [])):
            return None
        path = os.path.join(folder, "files", name)
        try:
            with open(path, "rb") as fh:
                head = fh.read(64)
            size = os.path.getsize(path)
        except OSError:
            return None
        kind = video_type(head)
        return (path, "video/" + kind, size) if kind else None

    def report_image(self, code, name):
        """(content-type, bytes) of a screenshot: the type comes from the bytes, not the name."""
        found = self.find_report(code)
        if not found:
            return None
        folder, meta = found
        if not any(f.get("name") == name and f.get("kind") == "screenshot" for f in meta.get("files", [])):
            return None
        try:
            with open(os.path.join(folder, "files", name), "rb") as fh:
                data = fh.read(MAX_REPORT_IMAGE + 1)
        except OSError:
            return None
        if len(data) > MAX_REPORT_IMAGE:
            return None
        kind = image_type(data[:16])
        return (IMAGE_TYPES[kind], data) if kind else None

    def report_file(self, code, name, limit=MAX_REPORT_TEXT):
        """Text of an attached file, tail-first when it is larger than `limit`."""
        found = self.find_report(code)
        if not found:
            return None
        folder, meta = found
        if name not in {f["name"] for f in meta.get("files", [])}:
            return None
        path = os.path.join(folder, "files", name)
        try:
            size = os.path.getsize(path)
            with open(path, "rb") as fh:
                if size > limit:
                    fh.seek(size - limit)
                data = fh.read(limit)
        except OSError:
            return None
        return {"name": name, "size": size, "truncated": size > limit,
                "text": data.decode("utf-8", "replace")}

    def mark_handled(self, code, handled=True):
        found = self.find_report(code)
        if not found:
            return {"ok": False, "error": "no such report"}
        folder, meta = found
        meta["handled"] = bool(handled)
        path = os.path.join(folder, "meta.json")
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump(meta, fh, indent=1, ensure_ascii=False)
        os.replace(tmp, path)
        return {"ok": True, "handled": bool(handled)}
