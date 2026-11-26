"""The socket loop of wslink: the same scripted clients against a bridge, once the Python one (tools/wslink.py) and once the Rust one
(t2r-daemon wslink). Prints one transcript line per step; both runs must print the same bytes.

  python3 wslink_loop_test.py py  <PYDIR> > py.out
  python3 wslink_loop_test.py rs  <path to t2r-daemon> > rs.out ; cmp py.out rs.out

A fake daemon (echo + a banner) listens on loopback and records the first line it is sent (the T2RFWD line). Steps: a plain WebSocket
round trip with CF-Connecting-IP, a ping, a fragmented payload, a 70 000 byte payload both ways, a daemon-side close, a client close,
a non-websocket request (404), a bad path (404), garbage, an unmasked frame, an oversize frame, a header block over 8 KB, the 503 when
the link limit is reached."""
import base64
import os
import socket
import struct
import subprocess
import sys
import threading
import time

MODE, ARG = sys.argv[1], sys.argv[2]


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


DP, WP = free_port(), free_port()
FIRST = []                     # (first line the fake daemon got) per accepted connection
MODEL = {"close_after": None}


def daemon_conn(c):
    try:
        c.settimeout(5)
        buf = b""
        while b"\n" not in buf:
            x = c.recv(1)
            if not x:
                return
            buf += x
        FIRST.append(buf.decode("latin-1"))
        c.sendall(b"HELLO\n")
        while True:
            d = c.recv(65536)
            if not d:
                return
            if d.startswith(b"BYE"):
                return
            c.sendall(d)
    except OSError:
        pass
    finally:
        try:
            c.close()
        except OSError:
            pass


def daemon():
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", DP))
    s.listen(16)
    while True:
        c, _ = s.accept()
        threading.Thread(target=daemon_conn, args=(c,), daemon=True).start()


threading.Thread(target=daemon, daemon=True).start()
if MODE == "py":
    cmd = [sys.executable, os.path.join(ARG, "wslink.py"), "--listen", "127.0.0.1:%d" % WP, "--daemon", "127.0.0.1:%d" % DP, "--max", "3"]
else:
    cmd = [ARG, "wslink", "--listen", "127.0.0.1:%d" % WP, "--daemon", "127.0.0.1:%d" % DP, "--max", "3"]
bridge = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
OUT = []


def out(*a):
    line = " ".join(str(x) for x in a)
    OUT.append(line)
    print(line)
    sys.stdout.flush()


def wait_port(p, t=20):
    end = time.time() + t
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", p), 0.5).close()
            return True
        except OSError:
            time.sleep(0.2)
    return False


def rd(s, n):
    b = b""
    while len(b) < n:
        x = s.recv(n - len(b))
        if not x:
            raise EOFError("closed")
        b += x
    return b


def head(s):
    h = b""
    while b"\r\n\r\n" not in h:
        x = s.recv(1)
        if not x:
            break
        h += x
    return h


def req(path="/link", ip="203.0.113.7", extra=b"", key="dGhlIHNhbXBsZSBub25jZQ==", upgrade=True):
    r = "GET %s HTTP/1.1\r\nHost: link\r\n" % path
    if upgrade:
        r += "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: %s\r\n" % key
    if ip:
        r += "CF-Connecting-IP: %s\r\n" % ip
    return r.encode() + extra + b"\r\n"


def mask_frame(op, data, fin=True, mask=b"\x01\x02\x03\x04"):
    n = len(data)
    b = bytes([(0x80 if fin else 0) | op])
    if n < 126:
        b += bytes([0x80 | n])
    elif n < 65536:
        b += bytes([0x80 | 126]) + struct.pack("!H", n)
    else:
        b += bytes([0x80 | 127]) + struct.pack("!Q", n)
    return b + mask + bytes(x ^ mask[i & 3] for i, x in enumerate(data))


def read_ws(s):
    b1, b2 = rd(s, 2)
    n = b2 & 0x7F
    if n == 126:
        n = struct.unpack("!H", rd(s, 2))[0]
    elif n == 127:
        n = struct.unpack("!Q", rd(s, 8))[0]
    return b1, rd(s, n)


def open_ws(**kw):
    c = socket.create_connection(("127.0.0.1", WP), 5)
    c.settimeout(5)
    c.sendall(req(**kw))
    h = head(c)
    return c, h


def step(name, f):
    try:
        out(name, f())
    except Exception as e:                       # noqa: BLE001
        out(name, "EXC", type(e).__name__, e)


def t_roundtrip():
    c, h = open_ws()
    st = h.split(b"\r\n")[0]
    acc = [l for l in h.split(b"\r\n") if l.lower().startswith(b"sec-websocket-accept")]
    b1, d = read_ws(c)                         # HELLO from the daemon
    c.sendall(mask_frame(2, b"ping-pong"))
    b3, d2 = read_ws(c)
    c.close()
    return [st, acc, b1, d, b3, d2]


def t_ping():
    c, h = open_ws(ip="2001:db8:0:0:0:0:0:7")
    read_ws(c)
    c.sendall(mask_frame(9, b"abc"))
    b1, d = read_ws(c)
    c.close()
    return [b1, d]


def t_fragment():
    c, h = open_ws(ip="")
    read_ws(c)
    c.sendall(mask_frame(2, b"aaa", fin=False) + mask_frame(0, b"bbb", fin=True))
    got = b""
    while len(got) < 6:
        b1, d = read_ws(c)
        got += d
    c.close()
    return got


def t_big():
    c, h = open_ws(ip="not-an-ip")
    read_ws(c)
    data = bytes(i % 251 for i in range(70000))
    c.sendall(mask_frame(2, data))
    got = b""
    while len(got) < len(data):
        b1, d = read_ws(c)
        got += d
    c.close()
    return got == data


def t_daemon_close():
    c, h = open_ws(ip="198.51.100.9")
    read_ws(c)
    c.sendall(mask_frame(2, b"BYE"))
    b1, d = read_ws(c)
    try:
        x = c.recv(10)
    except OSError as e:
        x = repr(e)
    c.close()
    return [b1, d, x]


def t_client_close():
    c, h = open_ws(ip="198.51.100.10")
    read_ws(c)
    c.sendall(mask_frame(8, b"\x03\xe8"))
    time.sleep(0.5)
    try:
        x = c.recv(10)
    except OSError as e:
        x = repr(e)
    c.close()
    return x


def t_plain_http():
    c, h = open_ws(upgrade=False)
    r = h.split(b"\r\n")[0]
    c.close()
    return r


def t_bad_path():
    c, h = open_ws(path="/other")
    r = h
    c.close()
    return r


def t_garbage():
    c = socket.create_connection(("127.0.0.1", WP), 5)
    c.settimeout(5)
    c.sendall(b"\x16\x03\x01\x00garbage\r\n\r\n")
    try:
        x = c.recv(100)
    except OSError as e:
        x = repr(e)
    c.close()
    return x


def t_post():
    c = socket.create_connection(("127.0.0.1", WP), 5)
    c.settimeout(5)
    c.sendall(b"POST /link HTTP/1.1\r\n\r\n")
    try:
        x = c.recv(100)
    except OSError as e:
        x = repr(e)
    c.close()
    return x


def t_unmasked():
    c, h = open_ws(ip="198.51.100.11")
    read_ws(c)
    c.sendall(bytes([0x82, 3]) + b"abc")
    time.sleep(0.5)
    try:
        x = c.recv(10)
    except OSError as e:
        x = repr(e)
    c.close()
    return x


def t_oversize():
    c, h = open_ws(ip="198.51.100.12")
    read_ws(c)
    c.sendall(bytes([0x82, 0x80 | 127]) + struct.pack("!Q", (1 << 20) + 1) + b"\0\0\0\0")
    time.sleep(0.5)
    try:
        x = c.recv(10)
    except OSError as e:
        x = repr(e)
    c.close()
    return x


def t_bighead():
    c = socket.create_connection(("127.0.0.1", WP), 5)
    c.settimeout(5)
    c.sendall(b"GET /link HTTP/1.1\r\nX: " + b"a" * 9000 + b"\r\n\r\n")
    try:
        x = c.recv(100)
    except OSError as e:
        x = repr(e)
    c.close()
    return x


def t_busy():
    conns = []
    res = []
    for i in range(4):
        c, h = open_ws(ip="192.0.2.%d" % (i + 1))
        res.append(h.split(b"\r\n")[0])
        conns.append(c)
    for c in conns:
        c.close()
    time.sleep(0.5)
    c, h = open_ws(ip="192.0.2.99")
    res.append(h.split(b"\r\n")[0])
    c.close()
    return res


try:
    if not wait_port(DP) or not wait_port(WP):
        out("ports", "not listening")
    else:
        for name, f in [("roundtrip", t_roundtrip), ("ping", t_ping), ("fragment", t_fragment), ("big", t_big), ("daemon_close", t_daemon_close),
                        ("client_close", t_client_close), ("plain_http", t_plain_http), ("bad_path", t_bad_path), ("garbage", t_garbage), ("post", t_post),
                        ("unmasked", t_unmasked), ("oversize", t_oversize), ("bighead", t_bighead), ("busy", t_busy)]:
            step(name, f)
        time.sleep(0.5)
        out("daemon first lines", sorted(FIRST))
finally:
    bridge.terminate()
    try:
        bout, _ = bridge.communicate(timeout=5)
    except Exception:                            # noqa: BLE001
        bridge.kill()
        bout = b""
    # the bridge's own log lines (time stamps cut), sorted: the order of two threads' lines is not fixed
    lines = sorted(l.decode("latin-1").split("] ", 1)[-1].replace(str(WP), "WP").replace(str(DP), "DP") for l in bout.splitlines() if l.strip())
    for l in lines:
        print("LOG", l)
