"""A connection whose socket fd is >= 1024 must still send. OutConn used select.select(), which raises ValueError for any
fd >= 1024 (FD_SETSIZE), so with a connection cap above ~900 every connection on such an fd died on its first send ("could not
reach the host", menu loops). The write check now uses poll().
Run on Linux: python3 tools/tests/test_send_fd_poll.py (on Windows only the small-fd part runs)."""
import os, socket, sys, threading, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
FAILS = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        FAILS.append(what)
s = socket.socket(); s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]; s.close()
os.environ.update(T2R_HOST="127.0.0.1", T2R_PORT=str(port))
import daemon  # noqa: E402

def recv_all(sock, n, timeout=5.0):
    sock.settimeout(timeout); got = b""
    try:
        while len(got) < n:
            b = sock.recv(65536)
            if not b: break
            got += b
    except OSError as e:
        print("   recv:", repr(e))
    return got

def run_pair(label):
    a, b = socket.socketpair()
    a.settimeout(120)                      # like a live connection after its first frame (send() then takes what fits)
    a.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4096)   # small kernel buffers: the big frame must go through the queue
    b.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    c = daemon.OutConn(a)
    fd = c.fileno()
    small = b"\x05\x00\xb0hello"
    try:
        c.sendall(small); ok = True
    except Exception as e:
        ok = False; print("   sendall raised", repr(e))
    check(ok and recv_all(b, len(small)) == small, "%s fd %d: small frame sent inline" % (label, fd))
    # a frame bigger than the kernel buffer: the rest goes through the writer thread (the 0.5 s wait path)
    big = os.urandom(daemon.SENDQ_MAX_BYTES // 2)
    try:
        c.sendall(big); ok = True
    except Exception as e:
        ok = False; print("   sendall raised", repr(e))
    got = recv_all(b, len(big), timeout=10)
    check(ok and got == big, "%s fd %d: %d KB frame delivered through the writer thread" % (label, fd, len(big) // 1024))
    check(not c._dead, "%s fd %d: connection still alive" % (label, fd))
    c.close(); b.close()
    return fd

run_pair("low")
if hasattr(daemon.select, "poll"):
    import resource
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    if soft < 2048:
        resource.setrlimit(resource.RLIMIT_NOFILE, (min(2048, hard), hard))
    filler = []
    while True:
        r, w = os.pipe(); filler += [r, w]
        if w >= 1030: break
    fd = run_pair("high")
    check(fd >= 1024, "high pair really got an fd >= 1024 (%d)" % fd)
    for x in filler: os.close(x)
else:
    print("skip high-fd part: no poll() on this OS")
print("ALL OK" if not FAILS else "FAILED: %d" % len(FAILS))
sys.exit(1 if FAILS else 0)
