"""Back up the full message history of the named channels (and their threads) via REST. Read-only.
   python export_hist.py <out_dir> <channel name>...   ; prints per-channel totals and counts before CUTOFF_UTC"""
import datetime
import json
import os
import sys
import time
import urllib.error
import urllib.request

c = json.load(open("config.json"))
G = c["guild_id"]
H = {"Authorization": "Bot " + c["token"], "User-Agent": "DiscordBot (twelveweeks, 1.0)"}
CUTOFF = os.environ.get("CUTOFF_UTC", "2026-11-20T02:03:34")
CUT_MS = int(datetime.datetime.fromisoformat(CUTOFF).replace(tzinfo=datetime.timezone.utc).timestamp() * 1000)


def get(u):
    while True:
        try:
            r = urllib.request.urlopen(urllib.request.Request("https://discord.com/api/v10" + u, headers=H), timeout=30)
            return json.load(r)
        except urllib.error.HTTPError as e:
            if e.code == 429:
                time.sleep(float(json.load(e).get("retry_after", 1)) + 0.2)
                continue
            raise


def ts(mid):
    return (int(mid) >> 22) + 1420070400000


def history(cid):
    out, before = [], None
    while True:
        page = get(f"/channels/{cid}/messages?limit=100" + (f"&before={before}" if before else ""))
        if not page:
            break
        out += page
        before = page[-1]["id"]
        time.sleep(0.3)
    return out[::-1]


def threads(cid, forum):
    ts_ = []
    act = get(f"/guilds/{G}/threads/active")
    ts_ += [t for t in act.get("threads", []) if t.get("parent_id") == cid]
    for kind in ("public",) + (() if forum else ("private",)):
        before = None
        while True:
            try:
                r = get(f"/channels/{cid}/threads/archived/{kind}?limit=100" + (f"&before={before}" if before else ""))
            except urllib.error.HTTPError:
                break
            ts_ += r.get("threads", [])
            if not r.get("has_more") or not r.get("threads"):
                break
            before = r["threads"][-1]["thread_metadata"]["archive_timestamp"]
    seen, uniq = set(), []
    for t in ts_:
        if t["id"] not in seen:
            seen.add(t["id"]); uniq.append(t)
    return uniq


def slim(m):
    return {"id": m["id"], "at": m["timestamp"], "author": m["author"].get("username"), "author_id": m["author"]["id"],
            "bot": m["author"].get("bot", False), "type": m.get("type"), "pinned": m.get("pinned"), "text": m.get("content"),
            "attachments": [a["url"] for a in m.get("attachments", [])], "embeds": m.get("embeds", []),
            "mentions": [u["id"] for u in m.get("mentions", [])]}


def main():
    out, names = sys.argv[1], sys.argv[2:]
    os.makedirs(out, exist_ok=True)
    chans = {x["name"]: x for x in get(f"/guilds/{G}/channels")}
    tot = {}
    for n in names:
        ch = chans[n]
        rec = {"channel": n, "id": ch["id"], "type": ch["type"], "cutoff_utc": CUTOFF, "messages": [], "threads": []}
        if ch["type"] != 15:
            rec["messages"] = [slim(m) for m in history(ch["id"])]
        for t in threads(ch["id"], ch["type"] == 15):
            rec["threads"].append({"id": t["id"], "name": t["name"], "created": ts(t["id"]),
                                   "messages": [slim(m) for m in history(t["id"])]})
        json.dump(rec, open(os.path.join(out, n + ".json"), "w", encoding="utf-8"), indent=1, ensure_ascii=False)
        allm = rec["messages"] + [m for t in rec["threads"] for m in t["messages"]]
        pre = [m for m in allm if ts(m["id"]) < CUT_MS]
        tot[n] = (len(allm), len(pre), sum(1 for m in pre if m["bot"]), len(rec["threads"]),
                  sum(1 for t in rec["threads"] if t["created"] < CUT_MS))
        print(f"#{n}: {tot[n][0]} messages, {tot[n][1]} before cutoff ({tot[n][2]} by bots), threads {tot[n][3]} ({tot[n][4]} made before cutoff)", flush=True)
    json.dump(tot, open(os.path.join(out, "_counts.json"), "w"), indent=1)


main()
