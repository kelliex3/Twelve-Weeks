"""Shared constants, helpers and base class for the admin dashboard store.

``store.py`` is a thin facade: the ``Admin`` methods live in responsibility
mixins (``store_live``, ``store_players``, ...). This module holds the pieces
every mixin needs and the small amount of infrastructure that used to sit at
the top of the monolithic store.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import sqlite3
import sys
import threading
import time
from typing import NamedTuple

# The shared tools live one level above admin_dashboard/. Resolve them from this
# file rather than the process CWD so the server works from any directory.
HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.dirname(HERE)
DASHBOARD = os.path.dirname(SERVER)
TOOLS = os.path.dirname(DASHBOARD)
for _path in (TOOLS, os.path.join(TOOLS, "update_server"), HERE):
    if _path not in sys.path:
        sys.path.insert(0, _path)

import clan  # noqa: E402
import logindex  # noqa: E402

try:
    import clan_story  # noqa: E402
except Exception:  # noqa: BLE001 - optional module
    clan_story = None
try:
    import clan_alife  # noqa: E402
except Exception:  # noqa: BLE001 - optional module
    clan_alife = None
try:
    import clan_view  # noqa: E402
    import clanfeed_view  # noqa: E402
except Exception:  # noqa: BLE001 - optional update_server modules
    clan_view = clanfeed_view = None

__all__ = [
    "HERE", "SERVER", "DASHBOARD", "TOOLS",
    "clan", "logindex", "clan_story", "clan_alife", "clan_view", "clanfeed_view",
    "SNAPSHOT_FRESH", "STATUS_AGE", "MATCH_SETUP_GRACE", "PROBE_CACHE", "LIVE_CACHE",
    "PLAYERS_CACHE", "CLANS_CACHE", "LOG_BYTES", "MAX_REPORT_TEXT", "MAX_REPORT_IMAGE",
    "NEWS_BACKUPS", "EARLY_DEPARTURES", "STATUS_NAMES", "MATCH_RESULTS", "STATE_ORDER",
    "IMAGE_TYPES", "REPORT_TEXT_SUFFIXES", "HEX_RUN", "HEX_DUMP", "LOG_NOISE", "LOG_WARN",
    "REPORT_CODE", "PLAYER_ID", "DATE", "PLAYER_COLUMNS", "CLAN_COLUMNS",
    "PlayerRow", "ClanRow", "redact", "pid_of", "pretty_level", "token_embedded_name",
    "normalize_name", "readable_text", "image_type", "video_type", "AdminBase",
]

# --------------------------------------------------------------------------- constants
SNAPSHOT_FRESH = 35.0                # a status file older than this is ignored
STATUS_AGE = 45.0                    # a STATUS log line older than this is stale
MATCH_SETUP_GRACE = 300.0            # a formed match that never started is closed after 5 min
PROBE_CACHE = 4.0
LIVE_CACHE = 2.0
PLAYERS_CACHE = 4.0
CLANS_CACHE = 10.0
LOG_BYTES = 3 * 1024 * 1024
MAX_REPORT_TEXT = 96 * 1024
MAX_REPORT_IMAGE = 8 * 1024 * 1024
NEWS_BACKUPS = 20

EARLY_DEPARTURES = ("timed out", "admin kick", "jip never loaded")
STATUS_NAMES = {0: "none", clan.ALIVE: "alive", clan.DEAD: "dead", clan.WON: "won"}
MATCH_RESULTS = {0: "TEAM 1 WON", 1: "TEAM 2 WON", 2: "DRAW"}
STATE_ORDER = {"match": 0, "queue": 1, "menu": 2}
IMAGE_TYPES = {"png": "image/png", "jpg": "image/jpeg", "webp": "image/webp", "gif": "image/gif", "bmp": "image/bmp"}
REPORT_TEXT_SUFFIXES = (".txt", ".log", ".json", ".md", ".ini", ".cfg", ".csv", ".xml")

HEX_RUN = re.compile(r"\b[0-9a-fA-F]{24,}\b")
HEX_DUMP = re.compile(r"(?:\b[0-9a-f]{2}\b ){6,}[0-9a-f]{2}\b(?: \.\.\.\(\+\d+B\))?")
LOG_NOISE = re.compile(r"^\[[\d:]+\] (TCP \(|UDP \(|RELAY-|UDP-|STATUS |PERF |DB backup ok|MATCHMAKE |SESSION )")
LOG_WARN = re.compile(r"timed out|silent|drop|refused|mismatch|stall|timeout|reaped|WARN|admin-kick|abort", re.I)
REPORT_CODE = re.compile(r"^R-[A-Z0-9]{4}$")
PLAYER_ID = re.compile(r"^[0-9a-f]{4,64}$")
DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")

PLAYER_COLUMNS = "token,name,kills,deaths,wins,matches,steamid,last_seen,parts_total,parts_n,draws"
CLAN_COLUMNS = "token_id,faction,status,day,healthy,hungry,sick,weeks_total,runs_won,runs_lost,last_result,meta,updated"


class PlayerRow(NamedTuple):
    token: str
    name: str | None
    kills: int
    deaths: int
    wins: int
    matches: int
    steamid: int
    last_seen: float
    parts_total: int
    parts_n: int
    draws: int


class ClanRow(NamedTuple):
    key: str
    faction: int
    status: int
    day: int
    healthy: int
    hungry: int
    sick: int
    weeks: int
    runs_won: int
    runs_lost: int
    last_result: str
    meta: str
    updated: float

# --------------------------------------------------------------------------- pure helpers
def redact(line: str) -> str:
    """Mask tokens: long hex strings and raw byte dumps (a token is 64 hex chars)."""
    return HEX_DUMP.sub("<bytes>", HEX_RUN.sub("<id>", line))


def pid_of(key: str) -> str:
    """Stable short id for a person key, safe to put in a URL."""
    return hashlib.sha256(key.encode()).hexdigest()[:10]


def pretty_level(level: str | None) -> str:
    if not level:
        return ""
    return re.sub(r"^(to1|to2)-", "", level).replace("-", " ").title()


def token_embedded_name(token: str) -> str:
    """The T2N: setup name inside a token (hex of b'T2N:' + name + 0), or ''."""
    try:
        raw = bytes.fromhex(token)
    except ValueError:
        return ""
    if not raw.startswith(b"T2N:"):
        return ""
    return raw[4:].split(b"\0")[0].decode("utf-8", "replace")


def normalize_name(name: str | None) -> str:
    return " ".join((name or "").lower().split())


def readable_text(value: object, limit: int, forbidden: str = "\x7f") -> str:
    """One printable line for kick reasons and ban comments (no control chars)."""
    text = "".join(ch for ch in str(value or "") if ch >= " " and ch not in forbidden)
    return re.sub(r"\s+", " ", text).strip()[:limit]


def image_type(head: bytes) -> str | None:
    if head[:8] == b"\x89PNG\r\n\x1a\n":
        return "png"
    if head[:3] == b"\xff\xd8\xff":
        return "jpg"
    if head[:4] == b"RIFF" and head[8:12] == b"WEBP":
        return "webp"
    if head[:6] in (b"GIF87a", b"GIF89a"):
        return "gif"
    if head[:2] == b"BM":
        return "bmp"
    return None


def video_type(head: bytes) -> str | None:
    if len(head) >= 12 and head[4:8] == b"ftyp":
        return "quicktime" if head[8:12] == b"qt  " else "mp4"
    if head[:4] == b"\x1a\x45\xdf\xa3":
        return "webm" if b"webm" in head else "x-matroska"
    return None

class AdminBase:
    """Cache, database and daemon-call infrastructure shared by every mixin."""

    def __init__(self, db, daemon_log="", reports="", daemon="", status_file="", admin_url="",
                 secret_file="", update_data="", launcher_dir=""):
        self.db = os.path.abspath(db)
        self.dbdir = os.path.dirname(self.db)
        self.reports = reports
        self.daemon_log = daemon_log
        self.daemon = daemon                  # "host:port" of the game port (probe only)
        self.status_file = status_file or os.path.join(self.dbdir, "t2r-status.json")
        self.admin_url = admin_url or "http://127.0.0.1:7011"
        self.secret_file = secret_file or os.path.join(self.dbdir, "admin-secret.txt")
        self.update_data = update_data
        self.launcher_dir = launcher_dir
        self.log = logindex.LogIndex(daemon_log) if daemon_log else None

        self.lock = threading.Lock()
        self.cache: dict[str, tuple[float, object]] = {}
        self.seen_online: dict[str, float] = {}      # pid -> when this site first saw it
        self.seen_state: dict[str, tuple[str, float]] = {}  # pid -> (state, since)
        self.live_state = {"t": 0.0, "v": None}
        self.probe_state = {"t": 0.0, "v": None}
        self._matches_cache = (None, [])            # (event fingerprint, derived matches)
        self._alife_summaries_cache: dict[str, tuple[float, dict]] = {}

    # ---------------------------------------------------------------- infrastructure
    def cached(self, key, ttl, loader):
        """Serve a fresh cached value, or a stale one marked `stale` if a refresh fails."""
        now = time.time()
        with self.lock:
            hit = self.cache.get(key)
            if hit and now - hit[0] < ttl:
                return hit[1]
        try:
            value = loader()
        except Exception as exc:  # noqa: BLE001 - a bad read keeps the previous value on screen
            with self.lock:
                hit = self.cache.get(key)
            if hit:
                stale = hit[1]
                if isinstance(stale, dict):
                    stale = dict(stale, stale=str(exc))
                return stale
            raise
        with self.lock:
            self.cache[key] = (now, value)
        return value

    def invalidate(self, *keys):
        with self.lock:
            for key in keys:
                self.cache.pop(key, None)

    def connect(self):
        """Read-only connection with a short timeout: the admin site never writes the DB."""
        conn = sqlite3.connect("file:%s?mode=ro" % self.db.replace("\\", "/"), uri=True, timeout=1.5)
        conn.execute("PRAGMA query_only=ON")
        return conn

    def read_json(self, name, default):
        try:
            with open(os.path.join(self.dbdir, name), encoding="utf-8") as fh:
                value = json.load(fh)
            return value if isinstance(value, type(default)) else default
        except (OSError, ValueError):
            return default
