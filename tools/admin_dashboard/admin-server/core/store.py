"""Data layer for the admin dashboard.

A readable rewrite of ``tools/admin_site/store.py`` (the Twelve-Weeks-Main worktree
is the behavioural reference). Method names, call signatures and JSON shapes stay
identical so the FastAPI layer and the classic client see the same data; the
implementation is split across responsibility mixins instead of one 1400-line file.

Sources, best first:
    live             daemon admin listener GET /live (needs admin-secret.txt by the DB)
    snapshot         <db dir>/t2r-status.json, written by the daemon every 10 s
    snapshot-counts  older snapshot with counts only; names come from the log
    log              reconstructed from daemon.log EVT / STATUS / ANNOUNCE lines

The game database is always opened read-only with a short timeout. Writes are
limited to bug-report flags, bans.txt, news.json and kick requests forwarded to
the daemon's loopback admin listener.
"""

from __future__ import annotations

import os
import sys

# Resolve the mixins and the shared tools from this file, not the process CWD.
HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.dirname(HERE)
DASHBOARD = os.path.dirname(SERVER)
TOOLS = os.path.dirname(DASHBOARD)
for _path in (TOOLS, os.path.join(TOOLS, "update_server"), HERE):
    if _path not in sys.path:
        sys.path.insert(0, _path)

from store_actions import ActionsMixin  # noqa: E402
from store_bans import BansMixin  # noqa: E402
from store_clan_admin import ClanAdminMixin  # noqa: E402
from store_clans import ClansMixin  # noqa: E402
from store_common import (  # noqa: E402
    CLANS_CACHE, DATE, EARLY_DEPARTURES, HEX_DUMP, HEX_RUN, IMAGE_TYPES, LIVE_CACHE,
    LOG_BYTES, LOG_NOISE, LOG_WARN, MATCH_RESULTS, MATCH_SETUP_GRACE, MAX_REPORT_IMAGE,
    MAX_REPORT_TEXT, NEWS_BACKUPS, PLAYER_ID, PLAYERS_CACHE, PLAYER_COLUMNS, CLAN_COLUMNS,
    PROBE_CACHE, REPORT_CODE, REPORT_TEXT_SUFFIXES, SNAPSHOT_FRESH, STATE_ORDER, STATUS_AGE,
    STATUS_NAMES, AdminBase, ClanRow, PlayerRow, clan, clan_alife, clan_story, clan_view,
    clanfeed_view, image_type, logindex, normalize_name, pid_of, pretty_level, readable_text,
    redact, token_embedded_name, video_type,
)
from store_logs import LogsMixin  # noqa: E402
from store_live import LiveMixin  # noqa: E402
from store_news import NewsMixin  # noqa: E402
from store_overview import OverviewMixin  # noqa: E402
from store_player_detail import PlayerDetailMixin  # noqa: E402
from store_players import PlayersMixin  # noqa: E402
from store_reports import ReportsMixin  # noqa: E402

__all__ = [
    "HERE", "SERVER", "DASHBOARD", "TOOLS",
    "clan", "logindex", "clan_story", "clan_alife", "clan_view", "clanfeed_view",
    "SNAPSHOT_FRESH", "STATUS_AGE", "MATCH_SETUP_GRACE", "PROBE_CACHE", "LIVE_CACHE",
    "PLAYERS_CACHE", "CLANS_CACHE", "LOG_BYTES", "MAX_REPORT_TEXT", "MAX_REPORT_IMAGE",
    "NEWS_BACKUPS", "EARLY_DEPARTURES", "STATUS_NAMES", "MATCH_RESULTS", "STATE_ORDER",
    "IMAGE_TYPES", "REPORT_TEXT_SUFFIXES", "HEX_RUN", "HEX_DUMP", "LOG_NOISE", "LOG_WARN",
    "REPORT_CODE", "PLAYER_ID", "DATE", "PLAYER_COLUMNS", "CLAN_COLUMNS",
    "PlayerRow", "ClanRow", "redact", "pid_of", "pretty_level", "token_embedded_name",
    "normalize_name", "readable_text", "image_type", "video_type", "Admin",
]


class Admin(
    AdminBase,
    LiveMixin,
    PlayersMixin,
    OverviewMixin,
    PlayerDetailMixin,
    ClansMixin,
    ReportsMixin,
    LogsMixin,
    ActionsMixin,
    NewsMixin,
    BansMixin,
    ClanAdminMixin,
):
    """One instance per server (live or test), holding its own caches and trackers."""

