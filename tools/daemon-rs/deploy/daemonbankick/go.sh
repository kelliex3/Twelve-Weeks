#!/bin/bash
# Ban kick: replace the live Rust TCP daemon binary (bin/t2r-daemon-tcp) with the memory-fix build (glibc, ping eviction fix, MEM lines)
# plus the ban auto-kick: when bans.txt is (re)loaded every online player it covers is kicked like an admin kick (left the match, back to
# the menu, ban text, connection closed; log "BAN-KICK <name> <key>"); a banned player's clan day books nothing, the others in that match
# get a protected day; an admin kick of a banned player is a ban kick. Staged in ~/t2r-live/staging/daemonrsbankick/.
# Run ONLY with the user's go. Rollback: bash ~/t2r-live/staging/daemonrsbankick/go_back.sh (one command).
# - guards: the live binary and run-daemon.sh must be what this build replaces (staged md5 values), the staged binary must be intact,
#   t2r-relay active, no server-relay match in the daemon.log tail (FORCE=1 overrides that last one)
# - run-daemon.sh is not changed (same env, same cwd host186, same T2R_DB_WAL=1 / T2R_TOOLS_DIR); bin/t2r-daemon (UDP relay) is not touched
set -e
cd $HOME/t2r-live
S=staging/daemonrsbankick
grep -qx "$(md5sum bin/t2r-daemon-tcp | cut -d' ' -f1)" $S/live-binary.md5 || { echo "bin/t2r-daemon-tcp is not a binary this build replaces (live-binary.md5 lists them), stop"; exit 1; }
[ "$(md5sum run-daemon.sh | cut -d' ' -f1)" = "$(cat $S/run-daemon.md5)" ] || { echo "run-daemon.sh changed since staging, stop"; exit 1; }
[ "$(md5sum $S/t2r-daemon-tcp | cut -d' ' -f1)" = "$(cat $S/t2r-daemon-tcp.md5)" ] || { echo "staged binary md5 wrong, stop"; exit 1; }
! ldd $S/t2r-daemon-tcp 2>&1 | grep -qE "not found|not a dynamic" || { echo "staged binary misses a shared library on this host, stop"; exit 1; }
grep -q 't2r-daemon-tcp serve' run-daemon.sh || { echo "run-daemon.sh does not start t2r-daemon-tcp, stop"; exit 1; }
systemctl is-active t2r-relay || { echo "t2r-relay not active, stop"; exit 1; }
D=$(cat host-dir)
R=$(tail -n 3000 $D/daemon.log | grep -ac "MATCH-NET server-relay" || true)
if [ "$R" != "0" ] && [ "${FORCE:-0}" != "1" ]; then echo "$R server-relay match line(s) in the daemon.log tail: wait, or FORCE=1"; exit 1; fi
cp -p bin/t2r-daemon-tcp bin/t2r-daemon-tcp.bak-bankick
install -m 755 $S/t2r-daemon-tcp bin/t2r-daemon-tcp.new
mv -f bin/t2r-daemon-tcp.new bin/t2r-daemon-tcp
L0=$(wc -l < $D/daemon.log)
sudo systemctl restart t2r-daemon
sleep 25
P=$(for p in $(pgrep -u kellie -f "t2r-daemon-tcp serve"); do [ "$(readlink /proc/$p/cwd)" = "$HOME/t2r-live/$D" ] && echo $p; done | head -1)
echo "daemon pid ${P:-NONE} binary $(md5sum bin/t2r-daemon-tcp | cut -c1-8)"
[ -n "$P" ] && { grep "open files" /proc/$P/limits; ps -o pid,etime,pcpu,rss,nlwp,cmd -p $P; }
systemctl is-active t2r-daemon t2r-relay
N=$(wc -l < $D/daemon.log); [ "$N" -ge "$L0" ] && F=$((L0 + 1)) || F=1
tail -n +$F $D/daemon.log > /tmp/daemonrsbankick-start.log
echo "start log: $(wc -l < /tmp/daemonrsbankick-start.log) lines; panic/HANDLER ERR/CONN THREAD ERR/Traceback: $(grep -aciE 'panic|HANDLER ERR|CONN THREAD ERR|Traceback' /tmp/daemonrsbankick-start.log)"
grep -aE "TCP LISTEN|daemon start|LIVE-STATE|re-adopt|DB check|journal" /tmp/daemonrsbankick-start.log | tail -8 | cut -c1-200
tail -n 5 $HOME/t2r-live/daemon.out | cut -c1-200
