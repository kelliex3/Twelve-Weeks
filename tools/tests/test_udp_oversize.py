"""Offline test: the daemon's UDP thread survives big datagrams.
A 4 KB datagram and a 60 KB datagram (both above the old 2048 B receive buffer, which raised WinError 10040 on Windows and
killed the thread) are sent to the real udp_server socket; then a normal packet must still reach UdpRelay.handle.
Run: python tools/tests/test_udp_oversize.py   (free localhost port, no port 7001/7101)."""
import os, sys, socket, threading, time
import sys as _t2s, os as _t2o; _t2s.path.append(_t2o.path.normpath(_t2o.path.join(_t2o.path.dirname(_t2o.path.abspath(__file__)), ".."))); import t2rtmp  # noqa: E402,F401  (temp hygiene: tools/t2rtmp.py)

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
FAILS = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: FAILS.append(what)

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]; s.close()
os.environ.update(T2R_HOST="127.0.0.1", T2R_PORT=str(port))
import daemon  # noqa: E402

seen = []
orig = daemon.UdpRelay.handle
def spy(self, data, addr, now=None):
    seen.append(len(data))
    return orig(self, data, addr, now)
daemon.UdpRelay.handle = spy

th = threading.Thread(target=daemon.udp_server, daemon=True)
th.start()
for _ in range(50):
    if daemon.UDP is not None: break
    time.sleep(0.05)
check(daemon.UDP is not None, "udp_server started")

c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
c.sendto(b"\x01" * 4096, ("127.0.0.1", port))
c.sendto(b"\x02" * 60000, ("127.0.0.1", port))
time.sleep(0.2)
check(th.is_alive(), "UDP thread alive after 4 KB + 60 KB datagrams")
c.sendto(b"\x03" * 40, ("127.0.0.1", port))
for _ in range(40):
    if 40 in seen: break
    time.sleep(0.05)
check(th.is_alive(), "UDP thread alive after the normal packet")
check(40 in seen, "normal 40 B packet reached UdpRelay.handle after the big ones (sizes seen: %s)" % seen)

print("FAILS:", len(FAILS))
sys.exit(1 if FAILS else 0)
