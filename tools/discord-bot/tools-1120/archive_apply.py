"""User 11-20: hide the pre-release history. Each named channel is renamed <name>-pre-release and moved into a staff-only,
read-only ARCHIVE category (history kept); a fresh empty channel with the same name, topic, permissions, category and position
takes its place. Nothing is deleted.   python archive_apply.py [--apply]   (without --apply: prints the plan only)"""
import json
import sys
import time
import urllib.error
import urllib.request
import uuid

c = json.load(open("config.json")); G = c["guild_id"]
H = {"Authorization": "Bot " + c["token"], "User-Agent": "DiscordBot (twelveweeks, 1.0)",
     "X-Audit-Log-Reason": "Archive pre-release history (owner request 11-20)"}
APPLY = "--apply" in sys.argv
NAMES = ["general", "off-topic", "wlf", "seraphites", "bug-reports", "clips"]
STAFF = ["Lead Dev", "Support Dev", "Moderator"]
VIEW, SEND, REACT, THREADS = 1 << 10, 1 << 11, 1 << 6, (1 << 35) | (1 << 36) | (1 << 38)
HIST = 1 << 16


def call(u, m="GET", body=None, files=None):
    while True:
        h = dict(H)
        data = None
        if files:
            b = uuid.uuid4().hex
            h["Content-Type"] = "multipart/form-data; boundary=" + b
            parts = [f'--{b}\r\nContent-Disposition: form-data; name="payload_json"\r\nContent-Type: application/json\r\n\r\n{json.dumps(body)}\r\n'.encode()]
            for i, (fn, blob) in enumerate(files):
                parts.append(f'--{b}\r\nContent-Disposition: form-data; name="files[{i}]"; filename="{fn}"\r\nContent-Type: image/png\r\n\r\n'.encode() + blob + b"\r\n")
            data = b"".join(parts) + f"--{b}--\r\n".encode()
        elif body is not None:
            h["Content-Type"] = "application/json"
            data = json.dumps(body).encode()
        try:
            r = urllib.request.urlopen(urllib.request.Request("https://discord.com/api/v10" + u, method=m, headers=h, data=data), timeout=30)
            t = r.read()
            return json.loads(t) if t else None
        except urllib.error.HTTPError as e:
            if e.code == 429:
                time.sleep(float(json.load(e).get("retry_after", 1)) + 0.3)
                continue
            raise RuntimeError(f"{m} {u}: {e.code} {e.read().decode()[:300]}")


def main():
    chans = call(f"/guilds/{G}/channels")
    by = {x["name"]: x for x in chans}
    roles = {r["name"]: r["id"] for r in call(f"/guilds/{G}/roles")}
    me = call("/users/@me")["id"]
    lock = [{"id": G, "type": 0, "allow": "0", "deny": str(VIEW)},
            {"id": me, "type": 1, "allow": str(VIEW | HIST), "deny": "0"}] + \
           [{"id": roles[n], "type": 0, "allow": str(VIEW | HIST), "deny": str(SEND | REACT | THREADS)} for n in STAFF if n in roles]
    for n in NAMES:
        assert n in by, n
        assert n + "-pre-release" not in by, n + "-pre-release already exists"
        x = by[n]
        print(f"#{n} ({x['id']}): rename -> #{n}-pre-release, move to ARCHIVE (staff read-only); new #{n} in the same place "
              f"with {len(x['permission_overwrites'])} overwrite(s), topic {x.get('topic')!r}")
    print("ARCHIVE:", "reuse" if "ARCHIVE" in by else "create", "| staff view:", [n for n in STAFF if n in roles])
    if not APPLY:
        print("PLAN ONLY")
        return
    arch = by.get("ARCHIVE") if by.get("ARCHIVE", {}).get("type") == 4 else None
    if arch is None:
        arch = call(f"/guilds/{G}/channels", "POST", {"name": "ARCHIVE", "type": 4, "permission_overwrites": lock})
        print("ARCHIVE created", arch["id"])
    made, positions = {}, []
    for n in NAMES:
        x = by[n]
        # 1. the new channel first (so the place never stands empty), same settings
        new = call(f"/guilds/{G}/channels", "POST", {
            "name": n, "type": x["type"], "topic": x.get("topic"), "nsfw": x.get("nsfw", False),
            "rate_limit_per_user": x.get("rate_limit_per_user", 0), "parent_id": x.get("parent_id"),
            "permission_overwrites": x["permission_overwrites"]})
        made[n] = new["id"]
        positions.append({"id": new["id"], "position": x["position"], "lock_permissions": False})
        # 2. the old one: renamed, locked, into ARCHIVE
        call(f"/channels/{x['id']}", "PATCH", {"name": n + "-pre-release", "parent_id": arch["id"], "permission_overwrites": lock,
                                                "topic": f"Messages from before the public release (archived 11-20). " + (x.get("topic") or "")[:900]})
        print(f"#{n}: new {new['id']}, old {x['id']} archived")
        time.sleep(0.5)
    call(f"/guilds/{G}/channels", "PATCH", positions)
    # the Seraphites header banner the old channel had
    if "seraphites" in made:
        call(f"/channels/{made['seraphites']}/messages", "POST",
             {"attachments": [{"id": 0, "filename": "banner-seraphites.png"}]},
             files=[("banner-seraphites.png", open("art/banner-seraphites.png", "rb").read())])
        print("#seraphites banner posted")
    # bot channel map
    try:
        cj = json.load(open("channels.json"))
    except Exception:
        cj = {}
    cj.update(made)
    json.dump(cj, open("channels.json", "w"), indent=1)
    json.dump({"new": made, "old": {n: by[n]["id"] for n in NAMES}, "archive": arch["id"]}, open("backups/prepublic-1120/_archive_map.json", "w"), indent=1)
    print("DONE", made)


main()
