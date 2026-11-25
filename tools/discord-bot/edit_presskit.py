"""Edit the #press-kit post buttons in place (REST PATCH, no gateway login). usage: PRESSKIT_CHANNEL_ID=.. PRESSKIT_MESSAGE_ID=.. python3 edit_presskit.py [--dry]"""
import json, os, sys, urllib.request
CH, MID = os.environ["PRESSKIT_CHANNEL_ID"], os.environ["PRESSKIT_MESSAGE_ID"]   # channel id + id of the post to edit
cfg = json.load(open("config.json"))
DL = "https://dl.twelve-weeks.com/download/"
buttons = [
    ("Press kit (.zip, 41 MB)", DL + "TwelveWeeks-PressKit.zip?v=2"),   # ?v= busts the Cloudflare copy of the old zip
    ("Trailer 1080p (.mp4)", DL + "TwelveWeeks-Trailer-1080p.mp4"),
    ("Trailer on YouTube", "https://youtu.be/z3GWSbrm5eI"),
]
for l, u in buttons:
    if u.startswith(DL):
        rq = urllib.request.Request(u, method="HEAD", headers={"User-Agent": "twelveweeks-bot"})
        with urllib.request.urlopen(rq, timeout=20) as r:
            print("ok", r.status, r.headers.get("Content-Length"), u)
payload = {"components": [{"type": 1, "components": [{"type": 2, "style": 5, "label": l, "url": u} for l, u in buttons]}]}
if "--dry" in sys.argv:
    print(json.dumps(payload, indent=1)); sys.exit(0)
rq = urllib.request.Request(f"https://discord.com/api/v10/channels/{CH}/messages/{MID}", data=json.dumps(payload).encode(), method="PATCH",
                            headers={"Authorization": "Bot " + cfg["token"], "Content-Type": "application/json", "User-Agent": "DiscordBot (twelveweeks, 1.0)"})
with urllib.request.urlopen(rq, timeout=30) as r:
    m = json.loads(r.read())
    print("edited", r.status, m["id"], [b["label"] for b in m["components"][0]["components"]], len(m.get("attachments", [])), "attachment")
