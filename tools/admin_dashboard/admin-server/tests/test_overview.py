"""Parity tests for the ported overview endpoint.

Run: python tools/admin_dashboard/admin-server/tests/test_overview.py
The Twelve-Weeks-Main worktree is the behavioural oracle; when that worktree is
absent, the in-tree tools/admin_site copy is used instead. The FastAPI payload
must match `store.Admin.overview()` key-for-key (apart from the `now` timestamp).
"""

import importlib.util
import os
import sqlite3
import sys
import tempfile
import time
import unittest
from pathlib import Path

SERVER = Path(__file__).resolve().parents[1]
DASHBOARD = SERVER.parent
REPO = DASHBOARD.parent.parent
MAIN = Path(os.environ.get("TWELVE_WEEKS_MAIN", str(REPO.parent / "Twelve-Weeks-Main")))
ORACLE = MAIN / "tools" / "admin_site"
if not (ORACLE / "store.py").is_file():
    ORACLE = REPO / "tools" / "admin_site"

# The oracle's fixture builder expects its own tools/ and update_server/ on
# sys.path. Put the dashboard server back first before importing its modules,
# since update_server also has a server.py.
sys.path.insert(0, str(SERVER))
sys.path.insert(0, str(ORACLE))
sys.path.insert(0, str(ORACLE.parent))
import test_admin  # noqa: E402  (oracle fixture builder)
sys.path.insert(0, str(SERVER))

from fastapi.testclient import TestClient  # noqa: E402

from config import Settings  # noqa: E402


def load_module(name: str, path: Path):
    """Import by absolute path so a same-named module earlier on sys.path cannot win."""
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


create_app = load_module("dashboard_server", SERVER / "server.py").create_app


class TestOverviewParity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.db = os.path.join(cls.tmp, "t2r.db")
        test_admin.make_db(cls.db)
        cls.log = os.path.join(cls.tmp, "daemon.log")
        with open(cls.log, "w", encoding="utf-8") as f:
            f.write(test_admin.log_text(time.time()))
        cls.settings = Settings(db=cls.db, daemon_log=cls.log, daemon="127.0.0.1:1", reports="")
        cls.client = TestClient(create_app(cls.settings))

        cls.test_db = os.path.join(cls.tmp, "test.db")
        test_admin.make_db(cls.test_db)
        with sqlite3.connect(cls.test_db) as conn:
            conn.execute("DELETE FROM players WHERE lower(name) <> 'alice'")
        cls.switch_settings = Settings(
            db=cls.db, daemon_log=cls.log, daemon="127.0.0.1:1", reports="",
            test_db=cls.test_db, test_daemon_log=cls.log, test_daemon="127.0.0.1:1",
        )
        cls.switch_client = TestClient(create_app(cls.switch_settings))

        cls.old_module = load_module("classic_store", ORACLE / "store.py")
        cls.old = cls.old_module.Admin(cls.db, cls.log, "", "127.0.0.1:1")

    def test_overview_matches_classic(self):
        response = self.client.get("/api/overview")
        self.assertEqual(response.status_code, 200, response.text)
        got = response.json()
        want = self.old.overview()
        got.pop("now", None)
        want.pop("now", None)
        self.assertEqual(got, want)

    def test_online_matches_classic(self):
        response = self.client.get("/api/online")
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(response.json(), self.old.players_online())

    def test_server_switch_payload(self):
        self.assertEqual(self.client.get("/api/server").json(), {"server": "live", "test": False})
        self.assertEqual(
            self.client.get("/api/server", cookies={"tw_srv": "test"}).json(),
            {"server": "live", "test": False},  # no test DB configured in this fixture
        )

    def test_live_test_switch_selects_the_other_database(self):
        self.assertEqual(self.switch_client.get("/api/server").json(), {"server": "live", "test": True})
        self.assertEqual(
            self.switch_client.get("/api/server", cookies={"tw_srv": "test"}).json(),
            {"server": "test", "test": True},
        )
        live = self.switch_client.get("/api/overview").json()
        test = self.switch_client.get("/api/overview", cookies={"tw_srv": "test"}).json()
        self.assertLess(test["totals"]["players"], live["totals"]["players"])

    def test_spa_fallback(self):
        response = self.client.get("/overview")
        self.assertIn(response.status_code, (200, 503))
        if response.status_code == 200:
            self.assertIn("<div id=\"root\">", response.text)


if __name__ == "__main__":
    unittest.main()
