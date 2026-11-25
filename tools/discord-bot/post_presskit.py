"""One-shot REST post of the #press-kit banner + link buttons (no gateway login; the running bot is untouched).
usage: PRESSKIT_CHANNEL_ID=.. python3 post_presskit.py <banner.png> [--dry]   (run in ~/twelveweeks-bot: reads config.json for the token)"""
import json, os, sys, urllib.request, uuid

CH = os.environ["PRESSKIT_CHANNEL_ID"]   # id of the press-kit channel
cfg = json.load(open("config.json"))
banner = sys.argv[1]
DL = "https://dl.twelve-weeks.com/download/"
buttons = [
    ("Press kit (.zip, 38 MB)", DL + "TwelveWeeks-PressKit.zip"),
    ("Trailer 1080p (.mp4)", DL + "TwelveWeeks-Trailer-1080p.mp4"),
    ("Trailer on YouTube", "https://youtu.be/z3GWSbrm5eI"),
]
payload = {
    "attachments": [{"id": 0, "filename": "banner-press-kit.png"}],
    "components": [{"type": 1, "components": [{"type": 2, "style": 5, "label": l, "url": u} for l, u in buttons]}],
    "allowed_mentions": {"parse": []},
}
for l, u in buttons:
    if u.startswith(DL):
        rq = urllib.request.Request(u, method="HEAD", headers={"User-Agent": "twelveweeks-bot"})
        with urllib.request.urlopen(rq, timeout=20) as r:
            print("ok", r.status, r.headers.get("Content-Length"), u)
if "--dry" in sys.argv:
    print(json.dumps(payload, indent=1)); sys.exit(0)
b = uuid.uuid4().hex
body = (f"--{b}\r\nContent-Disposition: form-data; name=\"payload_json\"\r\nContent-Type: application/json\r\n\r\n"
        + json.dumps(payload) + f"\r\n--{b}\r\nContent-Disposition: form-data; name=\"files[0]\"; filename=\"banner-press-kit.png\"\r\n"
        "Content-Type: image/png\r\n\r\n").encode() + open(banner, "rb").read() + f"\r\n--{b}--\r\n".encode()
rq = urllib.request.Request(f"https://discord.com/api/v10/channels/{CH}/messages", data=body, method="POST",
                            headers={"Authorization": "Bot " + cfg["token"], "Content-Type": "multipart/form-data; boundary=" + b,
                                     "User-Agent": "DiscordBot (twelveweeks, 1.0)"})
with urllib.request.urlopen(rq, timeout=30) as r:
    m = json.loads(r.read())
    print("posted", r.status, m["id"])
