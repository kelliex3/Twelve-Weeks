"""One-shot REST post of the build patch notes (no gateway login, so the running bot is untouched).
usage: python3 discord_post.py <parts.json> <channel name> [--dry] [--everyone]  (--everyone: @everyone on the first message only)
Reads the token + guild from config.json and the channel ids from channels.json next to this script (the bot's folder). Never prints the token."""
import json, os, sys, time, urllib.request

D = os.path.dirname(os.path.abspath(__file__))
cfg = json.load(open(os.path.join(D, "config.json")))
chans = json.load(open(os.path.join(D, "channels.json")))
parts = json.load(open(sys.argv[1], encoding="utf-8"))
name = sys.argv[2]
dry = "--dry" in sys.argv
cid = chans.get(name) if isinstance(chans, dict) else None
if isinstance(cid, dict):
    cid = cid.get("id")
if not cid:
    sys.exit("no channel id for %r in channels.json (keys: %s)" % (name, list(chans)[:20]))
COLOR = 0xC8B98F

# group embeds into messages: <= 10 embeds and <= 5800 chars per message
msgs, cur, n = [], [], 0
for p in parts:
    e = {"title": p["title"], "description": p["description"], "color": COLOR}
    sz = len(p["title"]) + len(p["description"])
    if cur and (n + sz > 5800 or len(cur) >= 10):
        msgs.append(cur); cur, n = [], 0
    cur.append(e); n += sz
if cur:
    msgs.append(cur)
print("channel", name, cid, "messages", len(msgs), [len(m) for m in msgs])
if dry:
    sys.exit(0)
ping = "--everyone" in sys.argv
for k, m in enumerate(msgs):
    msg = {"embeds": m, "allowed_mentions": {"parse": ["everyone"] if ping and k == 0 else []}}
    if ping and k == 0:
        msg["content"] = "@everyone"
    body = json.dumps(msg).encode()
    rq = urllib.request.Request("https://discord.com/api/v10/channels/%s/messages" % cid, data=body, method="POST",
                                headers={"Authorization": "Bot " + cfg["token"], "Content-Type": "application/json",
                                         "User-Agent": "DiscordBot (twelveweeks, 1.0)"})
    with urllib.request.urlopen(rq, timeout=20) as r:
        print("posted", r.status, json.loads(r.read())["id"])
    time.sleep(1.2)
