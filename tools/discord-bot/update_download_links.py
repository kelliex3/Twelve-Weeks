import json,os,re,sys,urllib.request,time
cfg=json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),"config.json")))
H={"Authorization":"Bot "+cfg["token"],"User-Agent":"DiscordBot (twelveweeks, 1.0)","Content-Type":"application/json"}
API="https://discord.com/api/v10"
def req(m,p,body=None):
    r=urllib.request.Request(API+p,method=m,headers=H,data=json.dumps(body).encode() if body is not None else None)
    for _ in range(5):
        try:
            with urllib.request.urlopen(r,timeout=20) as f: return json.loads(f.read() or b"null")
        except urllib.error.HTTPError as e:
            if e.code==429: time.sleep(float(json.loads(e.read()).get("retry_after",2))+0.5); continue
            raise
RE=re.compile(r"TwelveWeeks-Launcher-0\.1\.\d+")
me=req("GET","/users/@me")["id"]
chans=[c for c in req("GET","/guilds/%s/channels"%cfg["guild_id"]) if c["type"] in (0,5)]
apply="--apply" in sys.argv
NEW="TwelveWeeks-Launcher-"+next((a for a in sys.argv[1:] if a[0].isdigit()),"0.1.45")
for c in chans:
    try: msgs=req("GET","/channels/%s/messages?limit=100"%c["id"])
    except Exception as e: print("skip",c["name"],e); continue
    for m in msgs:
        s=json.dumps({"content":m.get("content"),"embeds":m.get("embeds"),"components":m.get("components")})
        hits=sorted(set(RE.findall(s)))
        if not hits: continue
        mine=m["author"]["id"]==me
        print(c["name"],m["id"],"bot" if mine else "author="+m["author"]["username"],hits,(m.get("content") or "")[:80].replace("\n"," "))
        if apply and mine and any(h!=NEW for h in hits):
            body={}
            if RE.search(m.get("content") or ""): body["content"]=RE.sub(NEW,m["content"])
            if m.get("components") and RE.search(json.dumps(m["components"])): body["components"]=json.loads(RE.sub(NEW,json.dumps(m["components"])))
            if m.get("embeds") and RE.search(json.dumps(m["embeds"])):
                body["embeds"]=[{k:v for k,v in e.items() if k not in ("type","video","provider")} for e in json.loads(RE.sub(NEW,json.dumps(m["embeds"])))]
            r=req("PATCH","/channels/%s/messages/%s"%(c["id"],m["id"]),body)
            print("  edited ->",sorted(set(RE.findall(json.dumps({"c":r.get("content"),"e":r.get("embeds"),"k":r.get("components")})))))
