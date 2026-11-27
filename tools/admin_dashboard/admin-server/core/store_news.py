"""Patch notes (news.json) validation and persistence."""

from __future__ import annotations

import json
import os
import shutil
import time

from store_common import DATE, NEWS_BACKUPS


class NewsMixin:
    """Reads, validates and saves patch notes."""

    # ---------------------------------------------------------------- patch notes (news.json)
    def news_path(self):
        return os.path.join(self.update_data, "news.json") if self.update_data else ""

    def news_get(self):
        path = self.news_path()
        if not path:
            return {"ok": False, "error": "the admin site was started without --update-data, so it cannot find news.json"}
        try:
            with open(path, encoding="utf-8") as fh:
                value = json.load(fh)
        except FileNotFoundError:
            value = []
        except (OSError, ValueError) as exc:
            return {"ok": False, "error": "news.json unreadable: %s" % exc}
        return {"ok": True, "entries": value if isinstance(value, list) else [], "path": path}

    @staticmethod
    def _news_sections(value, where):
        if not isinstance(value, dict):
            raise ValueError("%s: sections must be an object" % where)
        out = {}
        for key, lines in value.items():
            if not isinstance(key, str) or not isinstance(lines, list) or not all(isinstance(x, str) for x in lines):
                raise ValueError("%s: section %r must be a list of text lines" % (where, key))
            cleaned = [x.strip() for x in lines if x.strip()]
            if cleaned:
                out[key] = cleaned
        return out

    def news_validate(self, entries):
        if not isinstance(entries, list) or len(entries) > 500:
            raise ValueError("entries must be a list (max 500)")
        seen, out = set(), []
        for entry in entries:
            if not isinstance(entry, dict):
                raise ValueError("every entry must be an object")
            ident = entry.get("id")
            if isinstance(ident, bool) or not isinstance(ident, int):
                raise ValueError("every entry needs an integer id")
            if ident in seen:
                raise ValueError("duplicate id %d" % ident)
            seen.add(ident)
            for field in ("date", "build", "kind", "stamp", "headline"):
                if field in entry and not isinstance(entry[field], str):
                    raise ValueError("entry %d: %s must be text" % (ident, field))
            if not (entry.get("headline") or "").strip() and not entry.get("title"):
                raise ValueError("entry %d: the headline is empty" % ident)
            if entry.get("date") and not DATE.match(entry["date"]):
                raise ValueError("entry %d: date must be YYYY-MM-DD" % ident)
            cleaned = dict(entry)
            if "headline" in cleaned:
                cleaned["headline"] = cleaned["headline"].strip()
            if "sections" in cleaned:
                cleaned["sections"] = self._news_sections(cleaned["sections"], "entry %d" % ident)
            if "pl" in cleaned:
                polish = cleaned["pl"]
                if not isinstance(polish, dict):
                    raise ValueError("entry %d: pl must be an object" % ident)
                polish = dict(polish)
                if "headline" in polish:
                    if not isinstance(polish["headline"], str):
                        raise ValueError("entry %d: pl headline must be text" % ident)
                    polish["headline"] = polish["headline"].strip()
                if "sections" in polish:
                    polish["sections"] = self._news_sections(polish["sections"], "entry %d pl" % ident)
                cleaned["pl"] = polish
            out.append(cleaned)
        return out

    def news_save(self, entries):
        path = self.news_path()
        if not path:
            return {"ok": False, "error": "the admin site was started without --update-data"}
        try:
            validated = self.news_validate(entries)
        except ValueError as exc:
            return {"ok": False, "error": str(exc)}
        data = json.dumps(validated, ensure_ascii=False, indent=1) + "\n"
        with self.lock:
            backup = self._backup_news(path)
            tmp = path + ".tmp"
            with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(data)
                fh.flush()
                os.fsync(fh.fileno())
            os.replace(tmp, path)
        return {"ok": True, "count": len(validated), "backup": os.path.basename(backup), "entries": validated}

    @staticmethod
    def _backup_news(path):
        if not os.path.isfile(path):
            return ""
        folder = os.path.join(os.path.dirname(path), "news-backups")
        os.makedirs(folder, exist_ok=True)
        stamp = time.strftime("%Y-%m-%d-%H%M%S")
        backup = os.path.join(folder, "news-%s.json" % stamp)
        number = 1
        while os.path.exists(backup):
            number += 1
            backup = os.path.join(folder, "news-%s-%d.json" % (stamp, number))
        shutil.copy2(path, backup)
        old = sorted(f for f in os.listdir(folder) if f.startswith("news-") and f.endswith(".json"))
        for name in old[:-NEWS_BACKUPS]:
            try:
                os.remove(os.path.join(folder, name))
            except OSError:
                pass
        return backup
