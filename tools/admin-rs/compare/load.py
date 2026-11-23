import http.client, json, os, sys, threading, time

port, pid, secs, logdir = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
tabs = int(sys.argv[5]) if len(sys.argv) > 5 else 1
CLK = os.sysconf("SC_CLK_TCK")


def get(path):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=120)
    c.request("GET", path, headers={"Host": "127.0.0.1:%d" % port})
    r = c.getresponse()
    d = r.read()
    c.close()
    return d


def proc():
    with open("/proc/%d/stat" % pid) as f:
        st = f.read().rsplit(")", 1)[1].split()
    cpu = (int(st[11]) + int(st[12])) / CLK
    rss = hwm = 0
    with open("/proc/%d/status" % pid) as f:
        for ln in f:
            if ln.startswith("VmRSS:"):
                rss = int(ln.split()[1]) // 1024
            if ln.startswith("VmHWM:"):
                hwm = int(ln.split()[1]) // 1024
    return cpu, rss, hwm


src = open(os.path.join(logdir, "daemon.log.1"), "rb").read().splitlines(keepends=True)
stop = time.time() + secs


def appender():
    i = 0
    path = os.path.join(logdir, "daemon.log")
    while time.time() < stop:
        chunk = []
        size = 0
        while size < 8500:
            ln = src[i % len(src)]
            i += 1
            if ln.startswith(b"#"):
                continue
            ln = time.strftime("[%H:%M:%S]").encode() + ln[10:]
            chunk.append(ln)
            size += len(ln)
        with open(path, "ab") as f:
            f.write(b"".join(chunk))
        time.sleep(1)


ids = [p["id"] for p in json.loads(get("/api/players"))["players"]][:40]
pages = ["/api/players?q=", "/api/matches", "/api/clans", "/api/logs?level=all&file=0&quiet=1&n=400", "/api/reports"] + ["/api/player?id=" + i for i in ids[:5]]
threading.Thread(target=appender, daemon=True).start()
c0, r0, h0 = proc()
t0 = time.time()
n = 0
lat = []
samples = []


def tab(k):
    global n
    j = k
    while time.time() < stop:
        a = time.time()
        get("/api/overview")
        get("/api/online")
        get(pages[j % len(pages)])
        lat.append(time.time() - a)
        n += 1
        j += 1
        time.sleep(max(0.0, 3.0 - (time.time() - a)))


ts = [threading.Thread(target=tab, args=(k,)) for k in range(tabs)]
for t in ts:
    t.start()
while time.time() < stop:
    time.sleep(5)
    samples.append(proc()[1])
for t in ts:
    t.join()
c1, r1, h1 = proc()
lat.sort()
print(json.dumps({"port": port, "secs": round(time.time() - t0), "polls": n, "cpu_s": round(c1 - c0, 2), "cpu_pct": round(100 * (c1 - c0) / (time.time() - t0), 2),
                  "rss_start_mb": r0, "rss_end_mb": r1, "rss_max_sampled_mb": max(samples or [r1]), "hwm_mb": h1,
                  "lat_p50_ms": round(1000 * lat[len(lat) // 2]), "lat_p95_ms": round(1000 * lat[int(len(lat) * 0.95)])}))
