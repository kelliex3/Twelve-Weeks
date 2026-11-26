"""Differential test of daemon.py's _fwd_line (the 'T2RFWD <ip>\n' first line wslink puts on a loopback connection) against `t2r-daemon selftest fwd`.

  python3 fwd_harness.py gen SEED N > script.jsonl
  T2R_FIRST_FRAME_S=1 T2R_PY_DIR=... python3 fwd_harness.py run < script.jsonl > out.py.jsonl
  T2R_FIRST_FRAME_S=1 t2r-daemon selftest fwd < script.jsonl > out.rs.jsonl ; cmp ...

Every call opens a loopback connection, a client thread writes the scripted chunks (hex) with a pause between them, the server side runs `_fwd_line`
on the accepted socket; the result is [ip or None, hex of the bytes still readable afterwards (so a consumed line / a normal frame left untouched shows)].
"""
import os
import socket
import sys
import threading
import time

os.environ.setdefault("T2R_DB", "/tmp/fwdpy.db")
os.environ.setdefault("T2R_TOOLS_DIR", "/tmp/fwdtools")
import cseed  # noqa: E402
from cseed import emit, main, script  # noqa: E402

LINES = [b"T2RFWD 1.2.3.4\n", b"T2RFWD 203.0.113.77\n", b"T2RFWD ::1\n", b"T2RFWD 2001:DB8:0:0:0:0:0:1\n", b"T2RFWD fe80::1\n", b"T2RFWD ::ffff:1.2.3.4\n",
         b"T2RFWD 1:0:0:2:0:0:0:3\n", b"T2RFWD ::\n", b"T2RFWD 1::\n", b"T2RFWD 0:0:1:0:0:1:0:0\n", b"T2RFWD  9.9.9.9 \r\n", b"T2RFWD \t8.8.8.8\x0b\n",
         b"T2RFWD 01.2.3.4\n", b"T2RFWD 999.1.1.1\n", b"T2RFWD hello\n", b"T2RFWD \n", b"T2RFWD\n", b"T2RFWD 1.2.3.4", b"T2RFWD 1.2.\xff.4\n", b"T2RFWD \x1c1.2.3.4\n",
         b"T2RFWD 1.2.3.4\nrest", b"T2RFWD 1.2.3.4\n\x1c\x00\x01hello", b"T2RFW", b"T2R", b"\x1c\x00\x02\x01\x02", b"\x05", b"", b"T2RFWX 1.2.3.4\n",
         b"t2rfwd 1.2.3.4\n", b"T2RFWD " + b"1" * 70, b"T2RFWD " + b"1" * 70 + b"\n", b"T2RFWD 1.2.3.4\n" * 2]


def run():
    sys.path.insert(0, os.environ.get("T2R_PY_DIR", "."))
    import daemon as d
    n = 0
    for c in script():
        n += 1
        chunks = [bytes.fromhex(h) for h in c["chunks"]]
        delay, close = c.get("delay", 0.0), c.get("close", True)
        ls = socket.socket()
        ls.bind(("127.0.0.1", 0))
        ls.listen(1)
        port = ls.getsockname()[1]

        def client():
            s = socket.create_connection(("127.0.0.1", port))
            for ch in chunks:
                try:
                    s.sendall(ch)
                except OSError:
                    pass
                time.sleep(delay)
            if not close:
                time.sleep(1.8)
            s.close()
        th = threading.Thread(target=client)
        th.start()
        cs, _ = ls.accept()
        ip = d._fwd_line(cs)
        cs.settimeout(0.3)
        rest = b""
        try:
            while True:
                b = cs.recv(4096)
                if not b:
                    break
                rest += b
        except OSError:
            pass
        th.join()
        cs.close()
        ls.close()
        emit(n, [ip, rest.hex()], None)


def gen(r, n):
    for _ in range(n):
        x = r.random()
        if x < 0.7:
            data = r.choice(LINES)
            k = r.choice([1, 1, 1, 2, 3])
            cuts = sorted(r.sample(range(0, len(data) + 1), min(k - 1, len(data) + 1))) if k > 1 else []
            parts, last = [], 0
            for cpos in cuts + [len(data)]:
                parts.append(data[last:cpos])
                last = cpos
            parts = [p for p in parts if p] or [b""]
            c = {"chunks": [p.hex() for p in parts], "delay": r.choice([0.0, 0.02, 0.05]), "close": r.random() < 0.7}
        else:
            c = {"chunks": [r.choice(LINES).hex(), r.choice(LINES).hex()], "delay": 0.03, "close": True}
        yield c


if __name__ == "__main__":
    main(gen, run)
