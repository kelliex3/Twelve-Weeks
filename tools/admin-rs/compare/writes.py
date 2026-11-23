import glob, json, os, re, sys
from compare import PY, RS, req, check, report, results

B = os.environ.get("ADM_CMP", "/root/adm-cmp")
A = {"X-Admin-Request": "1", "Content-Type": "application/json"}


def post(path, body):
    return check("write %s %s" % (path, json.dumps(body)[:70]), "POST", path, A, json.dumps(body))


def players():
    _, _, d = req(PY, "GET", "/api/players?q=a")
    return json.loads(d)["players"]


def online_ids():
    _, _, d = req(PY, "GET", "/api/online")
    return [p.get("playerId") for p in json.loads(d)["players"] if p.get("playerId")]


ps = players()
with_sid = [p for p in ps if p["steamid"]]
no_sid = [p for p in ps if not p["steamid"]]
on = online_ids()
target_online = on[0] if on else with_sid[0]["id"]
post("/api/ban", {"id": target_online, "reason": "cheating # really\x07  a lot"})
post("/api/ban", {"id": target_online, "reason": "again"})
post("/api/ban", {"id": with_sid[3]["id"]})
if no_sid:
    post("/api/ban", {"id": no_sid[0]["id"], "reason": "name ban"})
check("write player after ban", "GET", "/api/player?id=" + target_online)
check("write players after ban", "GET", "/api/players?q=a")
post("/api/unban", {"id": with_sid[3]["id"]})
post("/api/unban", {"id": with_sid[3]["id"]})
_, _, d = req(PY, "GET", "/api/reports")
codes = [r["code"] for r in json.loads(d)["reports"]]
post("/api/report-handled", {"code": codes[0]})
post("/api/report-handled", {"code": codes[1], "handled": False})
post("/api/report-handled", {"code": codes[0].lower(), "handled": 0})
check("write reports after", "GET", "/api/reports")
_, _, d = req(PY, "GET", "/api/news")
entries = json.loads(d)["entries"]
mod = json.loads(json.dumps(entries))
if mod:
    mod[0]["headline"] = "  Edited headline \u00e9  "
    mod[0].setdefault("sections", {})["New"] = ["  line one ", "", "two"]
    mod[0]["pl"] = {"headline": " Polski ", "sections": {"A": [" x "]}}
mod.append({"id": 999999, "date": "2026-11-23", "headline": "added", "kind": "patch"})
post("/api/news", {"entries": mod})
post("/api/news", {"entries": mod[:1]})
check("write news after", "GET", "/api/news")
report("writes")


def read(p):
    try:
        with open(p, "rb") as f:
            return f.read()
    except OSError:
        return None


def strip_ts(b):
    return re.sub(rb"\d{4}-\d{2}-\d{2}( \d\d:\d\d:\d\d)?", b"<date>", b or b"")


same, diff = 0, []
files = ["live/data/bans.txt", "live/data/bans.txt.bak-admin", "live/update-data/news.json"]
files += [os.path.relpath(p, B + "/py") for p in glob.glob(B + "/py/live/update-data/reports/*/meta.json")]
for f in files:
    a, b = read(B + "/py/" + f), read(B + "/rs/" + f)
    if strip_ts(a) == strip_ts(b):
        same += 1
    else:
        diff.append(f)
a = strip_ts(read(B + "/py/live/data/admin-actions.log"))
b = strip_ts(read(B + "/rs/live/data/admin-actions.log"))
(diff.append("admin-actions.log") if a != b else None)
na = sorted(os.listdir(B + "/py/live/update-data/news-backups"))
nb = sorted(os.listdir(B + "/rs/live/update-data/news-backups"))
print("files: %d same, different: %s; news backups py %d rs %d" % (same, diff, len(na), len(nb)))
for f in diff:
    print("--", f)
    p = "live/data/admin-actions.log" if f == "admin-actions.log" else f
    print("py:", (read(B + "/py/" + p) or b"")[:600])
    print("rs:", (read(B + "/rs/" + p) or b"")[:600])
print(open(B + "/rs/live/data/bans.txt").read())
