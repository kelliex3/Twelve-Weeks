import re
import sys

OLD = rb'^(    )threading\.Thread\(target=supervised\(udp_server, "udp"\), daemon=True\)\.start\(\)(\r?\n)'


def main():
    path = sys.argv[1]
    data = open(path, "rb").read()
    if b'os.environ.get("T2R_UDP"' in data:
        print("already applied:", path)
        return 0
    hits = list(re.finditer(OLD, data, re.M))
    if len(hits) != 1:
        print("expected exactly one UDP thread start line, found %d: %s" % (len(hits), path))
        return 1
    m = hits[0]
    ind, nl = m.group(1), m.group(2)
    new = (ind + b'if os.environ.get("T2R_UDP", "1") != "0":' + nl
           + ind + b'    threading.Thread(target=supervised(udp_server, "udp"), daemon=True).start()' + nl
           + ind + b'else:' + nl
           + ind + b'    log("UDP off (T2R_UDP=0): the relay runs in t2r-daemon")' + nl)
    out = data[:m.start()] + new + data[m.end():]
    compile(out, path, "exec")
    open(path, "wb").write(out)
    print("applied:", path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
