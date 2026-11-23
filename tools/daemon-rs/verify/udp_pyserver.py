import collections
import os
import socket
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import udp_replay


def main():
    daemon_py, log_path = sys.argv[1], sys.argv[2]
    src = open(daemon_py, encoding="utf-8").read().replace("\r\n", "\n")
    a = src.index("\n# ---------------------------------------------------------------- UDP\n")
    b = src.index("\n# ---------------------------------------------------------------- reaper", a)
    q = collections.deque(maxlen=50000)
    _ctrl = udp_replay._CTRL

    def log(*x):
        q.append(f"[{time.strftime('%H:%M:%S')}] " + " ".join(str(v) for v in x).translate(_ctrl))

    def bind_exclusive(s, kind):
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((os.environ.get("T2R_HOST", "0.0.0.0"), int(os.environ.get("T2R_PORT", "7001"))))

    class Pres:
        def ping(self, addr):
            pass

    ns = dict(os=os, struct=struct, time=time, socket=socket, threading=threading, collections=collections, log=log,
              evt=lambda kind, *x: log("EVT", kind, *x), RELAY={}, RELAY_TS={}, UDP_ERRS=[0],
              RELAY_STATS=collections.Counter(), RELAY_TYPES=collections.Counter(), UDP_STATS=collections.Counter(),
              PRES=Pres(), peer_seen=lambda ip, addr: None, match_sid=lambda pl: 0, MM=None, bind_exclusive=bind_exclusive,
              HOST=os.environ.get("T2R_HOST", "0.0.0.0"), PORT=int(os.environ.get("T2R_PORT", "7001")), __file__=daemon_py)
    exec(compile(src[a:b], daemon_py + ":UDP", "exec"), ns)

    def flusher():
        while True:
            time.sleep(0.5)
            with open(log_path, "a", encoding="utf-8", errors="replace") as f:
                while q:
                    f.write(q.popleft() + "\n")

    def status():
        while True:
            time.sleep(10)
            if ns["UDP"] is not None:
                udp_replay.tick(ns, ns["UDP"], log)

    threading.Thread(target=flusher, daemon=True).start()
    threading.Thread(target=status, daemon=True).start()
    ns["udp_server"]()


if __name__ == "__main__":
    main()
