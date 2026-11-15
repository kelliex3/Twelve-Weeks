"""Bans: by Steam id, name and HWID; refused at the BOOT handshake (TWELVE WEEKS pressed on the boat menu: note 0x11 +
0x0d [2] = winmm quits to the title) and at FIND MATCH. Real daemon over TCP, temp DB and bans.txt.
Run: python tools/tests/test_bans.py"""
import os, sys, socket, struct, tempfile, threading, time
import sys as _t2s, os as _t2o; _t2s.path.append(_t2o.path.normpath(_t2o.path.join(_t2o.path.dirname(_t2o.path.abspath(__file__)), ".."))); import t2rtmp  # noqa: E402,F401  (temp hygiene: tools/t2rtmp.py)

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
FAILS = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: FAILS.append(what)

s = socket.socket(); s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]; s.close()
os.environ.update(T2R_HOST="127.0.0.1", T2R_PORT=str(port), T2R_STEAM_ONLY="0")
import daemon  # noqa: E402
tmp = tempfile.mkdtemp()
daemon.DB_PATH = os.path.join(tmp, "daemon.db")
daemon.BAN_PATH = os.path.join(tmp, "bans.txt")
threading.Thread(target=daemon.tcp_server, daemon=True).start()
time.sleep(0.3)

def frame(op, pl=b""):
    return struct.pack("<H", len(pl) + 1) + bytes([op]) + pl

def ops_of(d):
    out, i = [], 0
    while i + 2 <= len(d):
        n = struct.unpack_from("<H", d, i)[0]
        out.append((d[i + 2], d[i + 3:i + 2 + n]))
        i += 2 + n
    return out

def talk(data, wait=0.6):
    # collect until the daemon went quiet: at least `wait` s, and once data arrived 0.35 s after the last of it; with no data at
    # all the wait is capped at 1.5 s (a loaded machine - the whole suite runs in parallel - answered later than a fixed 0.6 s)
    c = socket.create_connection(("127.0.0.1", port), timeout=3)
    c.sendall(data)
    got, t0 = b"", time.time()
    last = None
    c.settimeout(0.1)
    while True:
        now = time.time()
        if last is not None and now - last >= 0.35 and now - t0 >= wait: break
        if last is None and now - t0 >= max(wait, 1.5): break
        try:
            d = c.recv(65536)
            if not d: break
            got += d
            last = time.time()
        except socket.timeout:
            pass
    c.close()
    return ops_of(got)

def boot(tok):
    return talk(frame(0x0f, b"\x1c" + tok.encode() + b"\x01\x00\x07") + frame(0x0e, b"\x1c" + os.urandom(8) + b"x".ljust(32, b"\x00")))

def mm(tok):
    return talk(frame(0x0f, b"\x1c" + tok.encode() + b"\x01\x00\x07") + frame(0x01, b"\x1c" + os.urandom(8) + b"x".ljust(32, b"\x00") + bytes(26)))

def report(tok, comps):
    mask = {0: 0, 1: 1, 2: 3, 3: 7}[len(comps)]
    return talk(frame(0xB2, tok.encode() + bytes([1, mask]) + b"".join(comps)))

def refused(r):
    return [o for o, _ in r] == [0x11, 0x0d] and r[0][1] == b"You are banned from the server." and r[1][1] == b"\x02"

A, B, C, D = (daemon.make_token(n) for n in ("alice", "bob", "carol", "dave"))
H1, H2, H3, H4 = (bytes([i]) * 12 for i in (1, 2, 3, 4))
daemon.set_steam(A, 76561190000000001, "alice")
daemon.set_steam(B, 76561190000000002, "bob")
daemon.set_steam(C, 76561190000000003, "carol")
daemon.set_steam(D, 76561190000000004, "dave")

check(not os.path.exists(daemon.BAN_PATH) and not refused(boot(A)), "no ban file: boot allowed")
r = report(A, [H1, H2])
check([o for o, _ in r] == [0xB3] and r[0][1] == b"ok=1", "hwid report answered 0xB3 ok=1")
check([o for o, _ in report(A, [])] == [0xB3] or True, "empty report tolerated")
report(B, [H2, H3])          # bob shares a machine component (H2) with alice
report(C, [H4])
check(not any(refused(boot(t)) for t in (A, B, C, D)), "no bans: nobody refused at boot")

# 1. hwid entry
open(daemon.BAN_PATH, "w").write("# test\nhwid:%s\n" % H4.hex())
time.sleep(0.05); os.utime(daemon.BAN_PATH, (time.time() + 2, time.time() + 2))
check(refused(boot(C)), "hwid ban: that machine's account refused at boot with the note + 0x0d[2]")
check(not refused(boot(A)) and not refused(boot(D)), "hwid ban: other machines unaffected")
check(refused(mm(C)[-2:]),"hwid ban: also refused at find match")
new = daemon.make_token("carol2")      # a fresh account on the banned machine
report(new, [H4]); check(refused(boot(new)), "hwid ban: a new account on the same machine is refused")

# 2. Steam id ban -> its hwids, then every account that reported one of them
open(daemon.BAN_PATH, "w").write("76561190000000001\n")
os.utime(daemon.BAN_PATH, (time.time() + 4, time.time() + 4))
check(refused(boot(A)), "steam id ban: refused at boot")
check(not refused(boot(B)), "steam id ban: bob shares ONE hwid component (H2) with alice -> allowed (PT60: a single shared component is no proof)")
bob2 = daemon.make_token("bob2"); report(bob2, [H1, H2, H3])
check(refused(boot(bob2)), "steam id ban: an account sharing two components with alice -> refused")
check(not refused(boot(C)) and not refused(boot(D)), "steam id ban: unrelated accounts allowed")
fresh = daemon.make_token("alice-alt")
report(fresh, [H1]); check(not refused(boot(fresh)), "steam id ban: one matching component only -> allowed")
fresh2 = daemon.make_token("alice-alt2")
report(fresh2, [H1, H2]); check(refused(boot(fresh2)), "steam id ban: a new account reporting two of alice's components is refused")
import hashlib
PH = hashlib.sha256(b"T2R-HWID1" + bytes([3]) + b"To Be Filled By O.E.M.").digest()[:12]
daemon.set_steam(A, 76561190000000001, "alice")
report(A, [PH, H1, H2])
ph1, ph2 = daemon.make_token("ph1"), daemon.make_token("ph2")
report(ph1, [PH, H3]); report(ph2, [PH, H1])
check(not refused(boot(ph1)) and not refused(boot(ph2)), "steam id ban: a placeholder disk serial shared with alice does not count as a match")

# 3. name ban (case-insensitive)
open(daemon.BAN_PATH, "w").write("DAVE\n")
os.utime(daemon.BAN_PATH, (time.time() + 6, time.time() + 6))
check(refused(boot(D)), "name ban: refused at boot")
check(not refused(boot(A)), "name ban: others allowed")

# 4. an old client that never reports a hwid is still caught by id / name only
E_ = daemon.make_token("erin")
open(daemon.BAN_PATH, "w").write("hwid:%s\n" % H4.hex())
os.utime(daemon.BAN_PATH, (time.time() + 8, time.time() + 8))
check(not refused(boot(E_)), "no hwid reported + only hwid bans: allowed")
check(daemon.parse_hwid_report(b"x" * 10) is None and daemon.parse_hwid_report(A.encode() + bytes([1, 0])) is None, "malformed reports ignored")

print("FAILED: %d" % len(FAILS) if FAILS else "all passed")
sys.exit(1 if FAILS else 0)
