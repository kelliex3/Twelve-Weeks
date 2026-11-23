import json, socket, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

B = sys.argv[1]
LOG = []

def status(which):
    with open("%s/base/%s/data/t2r-status.json" % (B, which)) as f:
        return json.load(f)

def make(which):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass
        def reply(self, code, obj):
            b = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)
        def ok(self):
            if self.headers.get("X-Admin-Secret") != "testsecret":
                self.reply(403, {"ok": False, "error": "bad secret"})
                return False
            return True
        def do_GET(self):
            if not self.ok():
                return
            u = urlparse(self.path)
            if u.path == "/live":
                d = status(which)
                return self.reply(200, {"ok": True, "server_version": d.get("server_version"), "live": d["live"]})
            if u.path == "/notices":
                return self.reply(200, {"ok": True, "notices": [{"id": 3, "kind": "t", "text": "hello " + which, "endsAt": 1795409021.5}]})
            if u.path == "/teams":
                return self.reply(200, {"ok": True, "rules": "apart a b\n", "preview": {"t0": ["x"], "t1": ["y"], "queue": ["x", "y"]}, "live": []})
            if u.path == "/clan/backups":
                return self.reply(200, {"ok": True, "backups": [{"file": "b1.json", "ts": 1795400000.0, "rows": {"clans": 1}}], "q": parse_qs(u.query)})
            self.reply(404, {"ok": False})
        def do_POST(self):
            if not self.ok():
                return
            n = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(n) or b"{}")
            LOG.append((which, self.path, body))
            self.reply(200, {"ok": True, "path": self.path, "got": body, "name": "N", "did": ["x"], "backup": "bk", "restored": 2})
    return H

def game(port):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(50)
    while True:
        c, _ = s.accept()
        c.close()

for port, which in ((17011, "live"), (17111, "test")):
    srv = ThreadingHTTPServer(("127.0.0.1", port), make(which))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
for port in (17001, 17101):
    threading.Thread(target=game, args=(port,), daemon=True).start()
print("fake daemon up", flush=True)
threading.Event().wait()
