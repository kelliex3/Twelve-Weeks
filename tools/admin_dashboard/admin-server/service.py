"""FastAPI-facing wrapper around the ported, framework-free data layer.

The JSON returned by these methods is intentionally the legacy ``tools/admin_site`` shape so the
React pages can consume it without a translation layer.
"""

from __future__ import annotations

import os

from config import Settings
from core import store


class AdminService:
    def __init__(self, settings: Settings):
        self.settings = settings
        self.live = store.Admin(
            str(settings.db), settings.daemon_log, settings.reports, settings.daemon,
            settings.status_file, settings.admin_url, "", settings.update_data, settings.launcher_dir,
        )
        self.test = None
        if settings.test_db and os.path.isfile(settings.test_db):
            self.test = store.Admin(
                settings.test_db, settings.test_daemon_log, settings.reports, settings.test_daemon,
                "", settings.test_admin_url, "", settings.update_data, settings.launcher_dir,
            )

    def for_request(self, test: bool = False):
        """The LIVE / TEST switch is a browser cookie (``tw_srv=test``) in the classic panel."""
        return self.test if test and self.test else self.live

    def server_info(self, test: bool = False) -> dict:
        return {"server": "test" if test and self.test else "live", "test": self.test is not None}
