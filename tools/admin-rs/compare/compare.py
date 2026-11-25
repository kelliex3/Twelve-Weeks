import base64, hashlib, hmac, http.client, json, sys, time
from urllib.parse import quote

PY, RS = 17010, 17020
TIMEY = {"now", "ms", "age_s", "online_s", "state_s", "uptime_s", "snapshotAge", "idle_s", "queued_s", "waited_s"}
DROPPED_PLAYER = {"people", "alive", "alifeServerOn", "alife"}
results = {"same": 0, "diff": []}
ONLY = ""


def req(port, method, path, headers=None, body=None, host=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=300)
    h = {"Host": host or "127.0.0.1:%d" % port}
    h.update(headers or {})
    c.request(method, path, body=body, headers=h)
    r = c.getresponse()
    data = r.read()
    hd = {k.lower(): v for k, v in r.getheaders()}
    c.close()
    return r.status, hd, data


def norm(v):
    if isinstance(v, str):
        return v.replace("/adm-cmp/py/", "/adm-cmp/X/").replace("/adm-cmp/rs/", "/adm-cmp/X/")
    if isinstance(v, dict):
        return {k: ("<t>" if k in TIMEY and isinstance(x, (int, float)) and not isinstance(x, bool) else norm(x)) for k, x in v.items()}
    if isinstance(v, list):
        return [norm(x) for x in v]
    return v


def body_of(hd, data):
    ct = hd.get("content-type", "")
    if "json" in ct:
        try:
            return json.loads(data)
        except ValueError:
            return ("badjson", data[:200])
    if ct.startswith("text/") or "html" in ct:
        return data.decode("utf-8", "replace")
    return ("bytes", len(data), hashlib.sha256(data).hexdigest()[:16])


def fix_py(path, b):
    if path.startswith("/api/player?") and isinstance(b, dict):
        for k in DROPPED_PLAYER:
            b.pop(k, None)
        b["alife"] = None if b.get("ok") else b.get("alife")
        if not b.get("ok"):
            b.pop("alife", None)
        for m in b.get("feed", []):
            m.pop("textPl", None)
    return b


KEEP_HEADERS = ("content-type", "cache-control", "content-security-policy", "x-content-type-options", "referrer-policy", "location",
                "www-authenticate", "content-range", "accept-ranges", "content-disposition", "content-length")


def check(name, method, path, headers=None, body=None, host_tpl=None, py_fix=True):
    if ONLY and ONLY not in name:
        return None
    hp = host_tpl % PY if host_tpl and "%d" in host_tpl else host_tpl
    hr = host_tpl % RS if host_tpl and "%d" in host_tpl else host_tpl
    sp, hp_, dp = req(PY, method, path, headers, body, hp)
    sr, hr_, dr = req(RS, method, path, headers, body, hr)
    bp = body_of(hp_, dp)
    br = body_of(hr_, dr)
    if py_fix:
        bp = fix_py(path, bp)
    bp, br = norm(bp), norm(br)
    if isinstance(bp, str):
        bp = bp.replace(":%d" % PY, ":PORT")
    if isinstance(br, str):
        br = br.replace(":%d" % RS, ":PORT")
    problems = []
    if sp != sr:
        problems.append("status %s vs %s" % (sp, sr))
    if bp != br:
        problems.append("body")
    for k in KEEP_HEADERS:
        a, b = hp_.get(k), hr_.get(k)
        if k == "content-length" and "json" in hp_.get("content-type", ""):
            continue
        if k == "cache-control" and a and b and a.startswith("no-store, ") and a[len("no-store, "):] == b:
            continue
        if (a or "").replace(str(PY), "PORT") != (b or "").replace(str(RS), "PORT"):
            problems.append("header %s: %r vs %r" % (k, a, b))
    if problems:
        results["diff"].append((name, problems, bp, br))
    else:
        results["same"] += 1
    return bp, br


def jdiff(a, b, path="", out=None, limit=12):
    out = [] if out is None else out
    if len(out) >= limit:
        return out
    num = (int, float)
    if type(a) != type(b) and not (isinstance(a, num) and isinstance(b, num) and not isinstance(a, bool) and not isinstance(b, bool)):
        out.append("%s: %r != %r" % (path, str(a)[:150], str(b)[:150]))
    elif isinstance(a, dict):
        for k in list(a.keys()) + [k for k in b if k not in a]:
            if k not in a or k not in b:
                out.append("%s/%s: only in %s" % (path, k, "py" if k in a else "rs"))
            else:
                jdiff(a[k], b[k], path + "/" + k, out, limit)
    elif isinstance(a, list):
        if len(a) != len(b):
            out.append("%s: len %d != %d" % (path, len(a), len(b)))
        for i, (x, y) in enumerate(zip(a, b)):
            jdiff(x, y, "%s[%d]" % (path, i), out, limit)
    elif a != b:
        out.append("%s: %r != %r" % (path, str(a)[:150], str(b)[:150]))
    return out


def cookie(sid, secret=b"websecret", exp=None):
    exp = exp or int(time.time()) + 3600
    mac = hmac.new(secret, ("%s.%d" % (sid, exp)).encode(), hashlib.sha256).hexdigest()
    return "tw_s=%s.%d.%s" % (sid, exp, mac)


READS = ["/api/whoami", "/api/server", "/", "/index.html", "/admin.css", "/admin.js", "/clan.html", "/clanframe.js", "/news.html", "/newsframe.js",
         "/icon.png", "/favicon.ico", "/launcher/src/clan.js", "/launcher/src/i18n/pl.json", "/src/style.css", "/launcher/src/app/news.js",
         "/launcher/assets/bg/wlf-hq.jpg", "/launcher/src/nope.js", "/nope", "/api/nope", "/static/admin.js", "/admin.js?x=1",
         "/api/overview", "/api/online", "/api/players", "/api/players?q=a", "/api/players?q=7656", "/api/players?q=zzzz", "/api/players?q=",
         "/api/notices", "/api/matches", "/api/teams", "/api/clans", "/api/reports", "/api/news",
         "/api/logs", "/api/logs?level=err", "/api/logs?level=warn&n=50", "/api/logs?level=evt&quiet=0", "/api/logs?q=match-end", "/api/logs?file=1",
         "/api/logs?file=5&n=20", "/api/logs?file=7", "/api/logs?file=x", "/api/logs?n=0&q=STATUS", "/api/logs?n=-3&q=STATUS&quiet=0", "/api/logs?n=abc",
         "/api/player?id=nope", "/api/clanview?id=nope", "/api/clan-backups?id=nope", "/api/report?code=nope", "/api/report?code=R-0000",
         "/login-token"]


def run_reads(tag, extra=None, players=True):
    H = dict(extra or {})
    for p in READS:
        check("%s GET %s" % (tag, p), "GET", p, H)
    ids = []
    if not players:
        return ids
    _, _, d = req(PY, "GET", "/api/players", H)
    ids = [p["id"] for p in json.loads(d)["players"]]
    _, _, d = req(PY, "GET", "/api/players?q=a", H)
    ids += [p["id"] for p in json.loads(d)["players"] if p["id"] not in ids]
    nclan = 0
    for n, i in enumerate(ids):
        check("%s player %s" % (tag, i), "GET", "/api/player?id=" + i, H)
        _, _, d = req(PY, "GET", "/api/player?id=" + i, H)
        if not json.loads(d).get("clan") and nclan < 5:
            nclan += 1
            check("%s clanview-noclan %s" % (tag, i), "GET", "/api/clanview?id=" + i, H)
        if n < 5:
            check("%s backups %s" % (tag, i), "GET", "/api/clan-backups?id=" + i, H)
    _, _, d = req(PY, "GET", "/api/reports", H)
    for r in json.loads(d).get("reports", []):
        code = r["code"]
        check("%s report %s" % (tag, code), "GET", "/api/report?code=" + code.lower(), H)
        _, _, d2 = req(PY, "GET", "/api/report?code=" + code, H)
        for f in json.loads(d2).get("files", [])[:8]:
            q = "code=%s&name=%s" % (code, quote(f["name"]))
            check("%s file %s/%s" % (tag, code, f["name"]), "GET", "/api/report-file?" + q, H)
            check("%s image %s/%s" % (tag, code, f["name"]), "GET", "/api/report-image?" + q, H)
            check("%s video %s/%s" % (tag, code, f["name"]), "GET", "/api/report-video?" + q, H)
            check("%s video-range %s/%s" % (tag, code, f["name"]), "GET", "/api/report-video?" + q, dict(H, Range="bytes=10-99"))
            check("%s video-suffix %s/%s" % (tag, code, f["name"]), "GET", "/api/report-video?" + q, dict(H, Range="bytes=-50"))
            check("%s video-416 %s/%s" % (tag, code, f["name"]), "GET", "/api/report-video?" + q, dict(H, Range="bytes=999999999-"))
        check("%s file-bad %s" % (tag, code), "GET", "/api/report-file?code=%s&name=../meta.json" % code, H)
    return ids


def basic(u):
    return "Basic " + base64.b64encode(u).decode()


def run_auth():
    T = "admin.twelve-weeks.com"
    check("auth tunnel no login", "GET", "/", host_tpl=T)
    check("auth tunnel api", "GET", "/api/overview", host_tpl=T)
    check("auth tunnel css", "GET", "/admin.css", host_tpl=T)
    check("auth tunnel icon", "GET", "/icon.png", host_tpl=T)
    check("auth tunnel login-token", "GET", "/login-token", host_tpl=T)
    check("auth tunnel basic ok", "GET", "/api/whoami", {"Authorization": basic(b"alice:tok123")}, host_tpl=T)
    check("auth tunnel basic ok2", "GET", "/api/whoami", {"Authorization": basic(b"bob:tok 456")}, host_tpl=T)
    check("auth tunnel basic ok login-token", "GET", "/login-token", {"Authorization": basic(b"alice:tok123")}, host_tpl=T)
    check("auth tunnel cookie admin", "GET", "/api/whoami", {"Cookie": cookie("76561190000000002")}, host_tpl=T)
    check("auth tunnel cookie admin page", "GET", "/", {"Cookie": "a=b; " + cookie("76561190000000002")}, host_tpl=T)
    check("auth tunnel cookie notadmin", "GET", "/", {"Cookie": cookie("76561190000000001")}, host_tpl=T)
    check("auth tunnel cookie expired", "GET", "/", {"Cookie": cookie("76561190000000002", exp=1000)}, host_tpl=T)
    check("auth tunnel cookie badmac", "GET", "/", {"Cookie": cookie("76561190000000002", secret=b"x")}, host_tpl=T)
    check("auth tunnel cookie junk", "GET", "/", {"Cookie": "tw_s=abc"}, host_tpl=T)
    check("auth tunnel cookie test switch", "GET", "/api/server", {"Cookie": cookie("76561190000000002") + "; tw_srv=test"}, host_tpl=T)
    check("auth bad host", "GET", "/", host_tpl="evil.com:%d")
    check("auth bad host2", "GET", "/", host_tpl="localhost:1")
    check("auth localhost", "GET", "/api/server", host_tpl="localhost:%d")
    check("auth bearer", "GET", "/api/whoami", {"Authorization": "Bearer x", "Cf-Connecting-Ip": "7.7.7.7"}, host_tpl=T)
    check("auth head", "HEAD", "/admin.js")
    check("auth head api", "HEAD", "/api/server")
    for i in range(11):
        check("auth tunnel basic bad %d" % i, "GET", "/api/whoami", {"Authorization": basic(b"alice:nope"), "Cf-Connecting-Ip": "9.9.9.9"}, host_tpl=T)
    check("auth tunnel locked good", "GET", "/api/whoami", {"Authorization": basic(b"alice:tok123"), "Cf-Connecting-Ip": "9.9.9.9"}, host_tpl=T)
    check("auth tunnel other ip ok", "GET", "/api/whoami", {"Authorization": basic(b"alice:tok123"), "Cf-Connecting-Ip": "8.8.8.8"}, host_tpl=T)


def run_posts(ids, tag):
    A = {"X-Admin-Request": "1", "Content-Type": "application/json"}
    T = "admin.twelve-weeks.com"
    cases = [
        ("/api/kick", {"pid": "abcd12", "reason": "  bad\tguy \x01 "}), ("/api/kick", {"pid": "XYZ"}), ("/api/kick", {}),
        ("/api/notice", {"text": "hi there", "seconds": 0}), ("/api/notice", {"text": "x" * 400, "seconds": "12.5"}), ("/api/notice", {"text": "x", "seconds": "abc"}),
        ("/api/notice-clear", {"id": 3}), ("/api/notice-clear", {"id": "4"}), ("/api/notice-clear", {"id": None}), ("/api/notice-clear", {}),
        ("/api/shutdown", {"minutes": 3000, "reason": "r" * 150}), ("/api/shutdown", {"minutes": -2, "restart": False}), ("/api/shutdown", {}),
        ("/api/shutdown-cancel", {}), ("/api/teams", {"rules": "apart a b"}), ("/api/teams-move", {"pid": "abcd"}), ("/api/teams-move", {"pid": "zz"}),
        ("/api/clan-backups", {}), ("/api/nope", {}), ("/api/clan-reset", {"id": ids[0], "confirm": "wrong"}),
        ("/api/clan-restore", {"id": "nope", "file": "x"}), ("/api/clan-restore", {"id": ids[1], "file": "b1.json"}),
        ("/api/report-handled", {"code": "R-0000"}), ("/api/ban", {"id": "nope"}), ("/api/unban", {"id": ids[2]}),
        ("/api/news", {"entries": "x"}), ("/api/news", {"entries": [{"id": 1, "headline": ""}]}),
    ]
    for path, body in cases:
        check("%s POST %s %s" % (tag, path, json.dumps(body)[:60]), "POST", path, A, json.dumps(body))
    check("%s POST no header" % tag, "POST", "/api/kick", {"Content-Type": "application/json"}, "{}")
    check("%s POST bad origin" % tag, "POST", "/api/kick", dict(A, Origin="https://evil.com"), "{}")
    check("%s POST bad json" % tag, "POST", "/api/kick", A, "{nope")
    check("%s POST empty" % tag, "POST", "/api/shutdown-cancel", A, "")
    check("%s POST tunnel no origin" % tag, "POST", "/api/shutdown-cancel", dict(A, Authorization=basic(b"alice:tok123")), "{}", host_tpl=T)
    check("%s POST tunnel http origin" % tag, "POST", "/api/shutdown-cancel", dict(A, Authorization=basic(b"alice:tok123"), Origin="http://admin.twelve-weeks.com"), "{}", host_tpl=T)
    check("%s POST tunnel ok" % tag, "POST", "/api/shutdown-cancel", dict(A, Authorization=basic(b"alice:tok123"), Origin="https://admin.twelve-weeks.com"), "{}", host_tpl=T)
    check("%s POST cookie no origin" % tag, "POST", "/api/shutdown-cancel", dict(A, Cookie=cookie("76561190000000002")), "{}", host_tpl=T)


def report(label):
    print("== %s: %d same, %d different" % (label, results["same"], len(results["diff"])))
    for name, problems, bp, br in results["diff"]:
        print("-- %s: %s" % (name, "; ".join(problems)))
        if "body" in problems:
            if isinstance(bp, (dict, list)) and isinstance(br, (dict, list)):
                for l in jdiff(bp, br):
                    print("     " + l)
            else:
                print("     py: %r" % (str(bp)[:300],))
                print("     rs: %r" % (str(br)[:300],))
    sys.stdout.flush()
    results["same"] = 0
    results["diff"] = []


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "live"
    ONLY = sys.argv[2] if len(sys.argv) > 2 else ""
    t0 = time.time()
    req(PY, "GET", "/api/overview")
    t1 = time.time()
    req(RS, "GET", "/api/overview")
    print("first overview: py %.2fs, rs %.2fs" % (t1 - t0, time.time() - t1))
    ids = run_reads(mode)
    report(mode + " reads")
    if mode == "live":
        run_reads("test", {"Cookie": "tw_srv=test"}, players=False)
        report("test-server reads")
        run_auth()
        report("auth")
        run_posts(ids, mode)
        report(mode + " posts")
