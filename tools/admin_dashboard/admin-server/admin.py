"""Compatibility shim for the early scaffold.

The old module here had CWD-relative secret/status paths and a Python 3.12-only f-string; the
implementation now lives in ``config.py``, ``service.py`` and ``core/``. Kept as an import path
for anything that still expects ``admin.Settings`` / ``admin.AdminService``.
"""

from config import Settings  # noqa: F401
from service import AdminService  # noqa: F401

__all__ = ["Settings", "AdminService"]
