"""Edit the bot's own last post in a channel whose first embed title matches. No new ping.
usage: python3 discord_edit.py <parts.json> <channel name> <title> [--dry]   (never prints the token)"""
import json, os, sys, urllib.request
D = os.path.dirname(os.path.abspath(__file__))
cfg = json.load(open(os.path.join(D, "config.json")))
chans = json.load(open(os.path.join(D, "channels.json")))
parts = json.load(open(sys.argv[1], encoding="utf-8")); name = sys.argv[2]; title = sys.argv[3]
cid = chans.get(name); cid = cid.get("id") if isinstance(cid, dict) else cid
H = {"Authorization": "Bot " + cfg["token"], "Content-Type": "application/json", "User-Agent": "DiscordBot (twelveweeks, 1.0)"}
def api(method, path, body=None):
    rq = urllib.request.Request("https://discord.com/api/v10" + path, data=json.dumps(body).encode() if body is not None else None, method=method, headers=H)
    with urllib.request.urlopen(rq, timeout=20) as r:
        return json.loads(r.read())
me = api("GET", "/users/@me")["id"]
msgs = api("GET", "/channels/%s/messages?limit=30" % cid)
hit = [m for m in msgs if m["author"]["id"] == me and m.get("embeds") and m["embeds"][0].get("title") == title]
has = [a[6:] for a in sys.argv if a.startswith("--has=")]
if has:
    hit = [m for m in hit if has[0] in m["embeds"][0].get("description", "")]
if len(hit) != 1:
    sys.exit("found %d matching bot messages for %r" % (len(hit), title))
m = hit[0]
embeds = [{"title": p["title"], "description": p["description"], "color": 0xC8B98F} for p in parts]
print("message", m["id"], "old len", len(m["embeds"][0].get("description", "")), "new len", len(embeds[0]["description"]))
if "--dry" in sys.argv:
    sys.exit(0)
r = api("PATCH", "/channels/%s/messages/%s" % (cid, m["id"]), {"embeds": embeds, "allowed_mentions": {"parse": []}})
print("edited", r["id"], "content kept:", repr(r.get("content")))
