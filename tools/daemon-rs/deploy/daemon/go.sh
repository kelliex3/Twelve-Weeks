#!/bin/bash
# Switch the live TCP daemon from python3 daemon.py to the Rust port (t2r-daemon serve). Staged in ~/t2r-live/staging/daemonrs/.
# Run ONLY with the user's go. Rollback: bash ~/t2r-live/staging/daemonrs/go_back.sh (one command).
# - md5 guards: run-daemon.sh and host186/*.py must be what this build was checked against (staged md5 lists), else stop.
# - the Rust binary is bin/t2r-daemon-tcp (bin/t2r-daemon stays the UDP relay of t2r-relay.service, not touched)
# - same cwd (host186), same env, same files (live-matches.json re-adopt, bans.txt, team_rules.txt, version.txt, t2r.db); T2R_TOOLS_DIR = that
#   folder (Python reads its data files next to daemon.py, the Rust binary next to itself unless told); T2R_DB_WAL=1 = the database in WAL mode,
#   backups from a read snapshot (no request waits on a backup)
# - relay: the UDP relay is its own process since 11-25 (t2r-relay.service), a daemon restart does not cut relay traffic; this script still
#   refuses while daemon.log shows a server-relay match in its tail (FORCE=1 overrides)
set -e
cd $HOME/t2r-live
S=staging/daemonrs
[ "$(md5sum run-daemon.sh | cut -d' ' -f1)" = "$(cat $S/run-daemon.md5)" ] || { echo "run-daemon.sh changed since staging, stop"; exit 1; }
D=$(cat host-dir)
(cd $D && md5sum -c --quiet ../$S/host-py.md5) || { echo "$D/*.py changed since staging (re-diff against the port first), stop"; exit 1; }
[ "$(md5sum $S/t2r-daemon-tcp | cut -d' ' -f1)" = "$(cat $S/t2r-daemon-tcp.md5)" ] || { echo "staged binary md5 wrong, stop"; exit 1; }
grep -q '^  python3 -u daemon.py ' run-daemon.sh || { echo "daemon start line not found, stop"; exit 1; }
systemctl is-active t2r-relay || { echo "t2r-relay not active, stop"; exit 1; }
R=$(tail -n 3000 $D/daemon.log | grep -ac "MATCH-NET server-relay" || true)
if [ "$R" != "0" ] && [ "${FORCE:-0}" != "1" ]; then echo "$R server-relay match line(s) in the daemon.log tail: wait, or FORCE=1"; exit 1; fi
cp -p run-daemon.sh run-daemon.sh.bak-daemonrs
install -m 755 $S/t2r-daemon-tcp bin/t2r-daemon-tcp
sed -i 's|^  python3 -u daemon.py |  T2R_DB_WAL=1 T2R_TOOLS_DIR=$PWD $HOME/t2r-live/bin/t2r-daemon-tcp serve |' run-daemon.sh
grep -n 't2r-daemon-tcp' run-daemon.sh
L0=$(wc -l < $D/daemon.log)
sudo systemctl restart t2r-daemon
sleep 25
P=$(for p in $(pgrep -u kellie -f "t2r-daemon-tcp serve"); do [ "$(readlink /proc/$p/cwd)" = "$HOME/t2r-live/$D" ] && echo $p; done | head -1)
echo "daemon pid ${P:-NONE}"
[ -n "$P" ] && { grep "open files" /proc/$P/limits; ps -o pid,etime,pcpu,rss,nlwp,cmd -p $P; }
systemctl is-active t2r-daemon t2r-relay
# the log since the restart (daemon.log may have rotated: then the whole new file)
N=$(wc -l < $D/daemon.log); [ "$N" -ge "$L0" ] && F=$((L0 + 1)) || F=1
tail -n +$F $D/daemon.log > /tmp/daemonrs-start.log
echo "start log: $(wc -l < /tmp/daemonrs-start.log) lines; panic/HANDLER ERR/CONN THREAD ERR/Traceback: $(grep -aciE 'panic|HANDLER ERR|CONN THREAD ERR|Traceback' /tmp/daemonrs-start.log)"
grep -aE "TCP LISTEN|daemon start|LIVE-STATE|re-adopt|DB check|journal" /tmp/daemonrs-start.log | tail -8 | cut -c1-200
tail -n 5 $HOME/t2r-live/daemon.out | cut -c1-200
