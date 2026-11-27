"""Configuration for the FastAPI admin backend.

All filesystem defaults are resolved from ``--db`` (or the repository root), never from the
process working directory: the old scaffold's ``../..`` paths silently pointed elsewhere when
uvicorn was started from a different folder.
"""

from __future__ import annotations

import argparse
import os
from dataclasses import dataclass, field
from pathlib import Path

SERVER_DIR = Path(__file__).resolve().parent
DASHBOARD_DIR = SERVER_DIR.parent
REPO_DIR = DASHBOARD_DIR.parent.parent
DEFAULT_CLIENT_DIST = DASHBOARD_DIR / "admin-client" / "dist"


def default_launcher_dir() -> str:
    p = REPO_DIR / "release" / "launcher" / "assets"
    return str(p) if p.is_dir() else ""


def default_db() -> Path:
    """The repo's local DB is a convenient dev default; production always passes --db."""
    return Path(os.environ.get("T2R_ADMIN_DB") or os.environ.get("T2R_DB") or (REPO_DIR / "tools" / "t2r.db"))


def default_daemon_log() -> str:
    v = os.environ.get("T2R_ADMIN_DAEMON_LOG", "")
    if v:
        return v
    local = REPO_DIR / "tools" / "daemon.log"
    return str(local) if local.is_file() else "auto"


@dataclass
class Settings:
    db: Path
    port: int = 7010
    bind: str = "127.0.0.1"
    daemon_log: str = ""
    reports: str = ""
    daemon: str = ""
    admin_url: str = "http://127.0.0.1:7011"
    status_file: str = ""
    update_data: str = ""
    users: str = ""
    tunnel_host: list[str] = field(default_factory=list)
    allow_host: list[str] = field(default_factory=list)
    web_secret: str = ""
    web_admins: str = ""
    site_url: str = "https://twelve-weeks.com"
    test_db: str = ""
    test_admin_url: str = "http://127.0.0.1:7111"
    test_daemon_log: str = ""
    test_daemon: str = "127.0.0.1:7101"
    launcher_dir: str = field(default_factory=default_launcher_dir)
    cors_origins: list[str] = field(default_factory=lambda: ["http://localhost:5173", "http://127.0.0.1:5173"])
    client_dist: Path = DEFAULT_CLIENT_DIST

    def __post_init__(self) -> None:
        self.db = Path(self.db).expanduser().resolve()
        ddir = self.db.parent
        self.status_file = self.status_file or str(ddir / "t2r-status.json")
        self.users = self.users or str(ddir / "admin-users.txt")
        self.web_secret = self.web_secret or str(ddir / "web-secret")
        self.web_admins = self.web_admins or str(ddir / "web-admins.txt")
        # optional dirs/files stay empty when absent; core.store treats them as "no data"
        if self.reports and not Path(self.reports).is_absolute():
            self.reports = str((ddir / self.reports).resolve())
        if self.update_data and not Path(self.update_data).is_absolute():
            self.update_data = str((ddir / self.update_data).resolve())

    @classmethod
    def from_env(cls) -> "Settings":
        return cls(
            db=default_db(),
            port=int(os.environ.get("T2R_ADMIN_PORT", "7010")),
            bind=os.environ.get("T2R_ADMIN_BIND", "127.0.0.1"),
            daemon_log=default_daemon_log(),
            reports=os.environ.get("T2R_ADMIN_REPORTS", ""),
            daemon=os.environ.get("T2R_ADMIN_DAEMON", ""),
            admin_url=os.environ.get("T2R_ADMIN_URL", "http://127.0.0.1:7011"),
            status_file=os.environ.get("T2R_ADMIN_STATUS_FILE", ""),
            update_data=os.environ.get("T2R_ADMIN_UPDATE_DATA", ""),
            test_db=os.environ.get("T2R_ADMIN_TEST_DB", ""),
            site_url=os.environ.get("T2R_ADMIN_SITE_URL", "https://twelve-weeks.com"),
        )

    @classmethod
    def from_args(cls, argv: list[str] | None = None) -> "Settings":
        ap = argparse.ArgumentParser(description="Twelve Weeks FastAPI admin dashboard")
        ap.add_argument("--db", default=str(default_db()), help="the daemon's t2r.db (opened read-only)")
        ap.add_argument("--port", type=int, default=7010)
        ap.add_argument("--bind", default="127.0.0.1")
        ap.add_argument("--daemon-log", default=default_daemon_log(), help="daemon.log ('auto' follows the newest under dist/)")
        ap.add_argument("--reports", default="", help="bug report store folder")
        ap.add_argument("--daemon", default="", help="host:port of the game port (default: from the log, else 127.0.0.1:7001)")
        ap.add_argument("--admin-url", default="http://127.0.0.1:7011", help="daemon loopback admin listener")
        ap.add_argument("--status-file", default="")
        ap.add_argument("--update-data", default="", help="update server data dir (channels.json / news.json)")
        ap.add_argument("--users", default="")
        ap.add_argument("--tunnel-host", action="append", default=[])
        ap.add_argument("--allow-host", action="append", default=[])
        ap.add_argument("--web-secret", default="")
        ap.add_argument("--web-admins", default="")
        ap.add_argument("--site-url", default="https://twelve-weeks.com")
        ap.add_argument("--test-db", default="")
        ap.add_argument("--test-admin-url", default="http://127.0.0.1:7111")
        ap.add_argument("--test-daemon-log", default="")
        ap.add_argument("--test-daemon", default="127.0.0.1:7101")
        a = ap.parse_args(argv)
        return cls(
            db=a.db, port=a.port, bind=a.bind, daemon_log=a.daemon_log, reports=a.reports,
            daemon=a.daemon, admin_url=a.admin_url, status_file=a.status_file,
            update_data=a.update_data, users=a.users, tunnel_host=a.tunnel_host,
            allow_host=a.allow_host, web_secret=a.web_secret, web_admins=a.web_admins,
            site_url=a.site_url, test_db=a.test_db, test_admin_url=a.test_admin_url,
            test_daemon_log=a.test_daemon_log, test_daemon=a.test_daemon,
        )
