import json, os, sys, time
B = sys.argv[1]
while True:
    for s in ("py", "rs"):
        for d in ("live", "test"):
            p = "%s/%s/%s/data/t2r-status.json" % (B, s, d)
            with open(p) as f:
                v = json.load(f)
            v["ts"] = time.time()
            with open(p + ".t", "w") as f:
                json.dump(v, f)
            os.replace(p + ".t", p)
    time.sleep(4)
