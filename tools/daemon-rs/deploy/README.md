# Stage 1 deployment (prepared, not applied)

The UDP relay moves out of `daemon.py` into `t2r-daemon relay`. TCP stays in Python.

Verified on sfo (2026-11-23) with the deployed `host186` code, `apply_udp_switch.py` applied and `T2R_UDP=0`:
- Python listens on TCP only and logs `UDP off (T2R_UDP=0): the relay runs in t2r-daemon`.
- The Rust relay listens on UDP of the same port number.
- A client's boot handshake gets the same reply as before (0x10, 0x08, 0x16, 0x14).
- Keepalives get their 0x04 pong and the fan-out copy from the Rust relay.

Steps, TEST daemon first (`t2r-test`, port 7101), each only with the user's OK:

1. Build: `MSYS_NO_PATHCONV=1 wsl -- bash /mnt/c/.../tools/daemon-rs/build.sh`. Copy `t2r-daemon` to `~kellie/t2r-test/bin/` (TEST) or `~kellie/t2r-live/bin/` (live), owner kellie, mode 755.
2. Apply the switch to that build's `daemon.py` with `python3 -I apply_udp_switch.py <path>/daemon.py`:
   - TEST: `host-dbfix/daemon.py`; live: `host186/daemon.py`. Keep a `.bak-relay` copy first.
   - The script rewrites the one line that starts the UDP thread, keeps its line ending, and compiles the result before writing.
   - It refuses to run unless that line appears exactly once, and does nothing if the switch is already there.
   - Tested on copies of both files; the TEST and live relay code is identical.
3. Start the relay:
   - live: install `t2r-relay.service` and start it;
   - TEST: a copy with `T2R_PORT=7101` and its own log path.
4. Add `export T2R_UDP=0` to that daemon's `run-daemon.sh` and restart the daemon at a quiet moment.
   - The restart drops the TCP sessions as any restart does; `live-matches.json` re-adopts running matches.
   - The port's UDP side is free only once Python restarts without its UDP thread. The relay retries the bind for 30 s (`T2R_BIND_RETRY_S`), then exits with code 3; systemd restarts it.
   - Unlike Python, the relay never sets `SO_REUSEADDR` on UDP. Otherwise Linux would let both processes share the port and split the traffic between them. Tested: while a reuse socket holds the port, the relay gets `EADDRINUSE` and binds once the port is free.
5. Check:
   - `relay.log` has `UDP LISTEN` and `UDP-HEALTH` lines every 10 s;
   - `daemon.log` has `UDP off`;
   - players' menus show the server as reachable.

Rollback:
- remove `T2R_UDP=0` from `run-daemon.sh`;
- stop the relay (`systemctl stop t2r-relay`);
- restart the daemon.

The patched `daemon.py` behaves exactly like the old one when `T2R_UDP` is unset.
