"""Twelve Weeks admin dashboard: FastAPI backend for the Refine client.

The overview payload is the legacy ``store.Admin.overview()`` dictionary verbatim (same keys,
same shapes), so the React page can render exactly what the classic ``viewOverview`` rendered.
"""

from __future__ import annotations

import json
from pathlib import Path

import uvicorn
from fastapi import FastAPI, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, HTMLResponse, Response
from pydantic import BaseModel

from config import Settings
from service import AdminService


def _json(obj, code: int = 200) -> Response:
    """Same serialisation as the classic admin site (``json.dumps(..., default=str)``)."""
    return Response(json.dumps(obj, default=str).encode("utf-8"), status_code=code, media_type="application/json; charset=utf-8")


def _test_request(request: Request) -> bool:
    return (request.cookies.get("tw_srv") or "") == "test"


class KickBody(BaseModel):
    pid: str
    reason: str = ""


class NoticeBody(BaseModel):
    text: str
    seconds: float = 8


class NoticeClearBody(BaseModel):
    id: int


class ShutdownBody(BaseModel):
    minutes: float = 5
    reason: str = ""
    restart: bool = True


def create_app(settings: Settings | None = None) -> FastAPI:
    settings = settings or Settings.from_env()
    service = AdminService(settings)
    app = FastAPI(title="Twelve Weeks admin", docs_url=None, redoc_url=None)
    app.state.settings = settings
    app.state.service = service

    app.add_middleware(
        CORSMiddleware,
        allow_origins=settings.cors_origins,
        allow_credentials=True,
        allow_methods=["*"],
        allow_headers=["*"],
    )

    @app.get("/api/health")
    def health():
        return {"ok": True, "server": "live"}

    @app.get("/api/whoami")
    def whoami():
        # auth is not ported yet; the classic page uses this for the header and the logout link.
        return _json({"kind": "local", "id": "", "name": "", "logout": "", "site": settings.site_url.rstrip("/")})

    @app.get("/api/server")
    def server(request: Request):
        return _json(service.server_info(_test_request(request)))

    @app.get("/api/overview")
    def overview(request: Request):
        try:
            return _json(service.for_request(_test_request(request)).overview())
        except Exception as e:  # noqa: BLE001 - one bad read must not take the panel down
            return _json({"error": "%s: %s" % (type(e).__name__, e)}, 500)

    @app.get("/api/online")
    def online(request: Request):
        try:
            return _json(service.for_request(_test_request(request)).players_online())
        except Exception as e:  # noqa: BLE001
            return _json({"error": "%s: %s" % (type(e).__name__, e)}, 500)

    @app.get("/api/notices")
    def notices(request: Request):
        try:
            return _json(service.for_request(_test_request(request)).notices())
        except Exception as e:  # noqa: BLE001
            return _json({"ok": False, "error": str(e), "notices": []})

    @app.post("/api/kick")
    def kick(body: KickBody, request: Request):
        return _json(service.for_request(_test_request(request)).kick(body.pid, body.reason))

    @app.post("/api/notice")
    def notice(body: NoticeBody, request: Request):
        return _json(service.for_request(_test_request(request)).notice_send(body.text, body.seconds))

    @app.post("/api/notice-clear")
    def notice_clear(body: NoticeClearBody, request: Request):
        return _json(service.for_request(_test_request(request)).notice_clear(body.id))

    @app.post("/api/shutdown")
    def shutdown(body: ShutdownBody, request: Request):
        return _json(service.for_request(_test_request(request)).shutdown_schedule(body.minutes, body.reason, body.restart))

    @app.post("/api/shutdown-cancel")
    def shutdown_cancel(request: Request):
        return _json(service.for_request(_test_request(request)).shutdown_cancel())

    # ---- built client (SPA) -------------------------------------------------
    dist: Path = settings.client_dist
    index = dist / "index.html"

    @app.get("/{full_path:path}")
    def spa(full_path: str):
        if full_path.startswith("api/"):
            return _json({"error": "not found"}, 404)
        if full_path and dist.is_dir():
            candidate = (dist / full_path).resolve()
            if candidate.is_relative_to(dist.resolve()) and candidate.is_file():
                return FileResponse(candidate)
        if index.is_file():
            return FileResponse(index)
        return HTMLResponse("<h1>admin client is not built</h1><p>cd admin-client && npm run build</p>", status_code=503)

    return app


app = create_app()


def main(argv: list[str] | None = None) -> None:
    settings = Settings.from_args(argv)
    application = create_app(settings)
    uvicorn.run(application, host=settings.bind, port=settings.port, headers=[("server", "t2r-admin/1")])


if __name__ == "__main__":
    main()
