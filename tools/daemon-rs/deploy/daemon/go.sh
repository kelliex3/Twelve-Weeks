#!/bin/bash
# Switch the live TCP daemon from python3 daemon.py to the Rust port (t2r-daemon serve). Staged in ~/t2r-live/staging/daemonrs/.
# Run ONLY with the user's go. Rollback: go_back.sh (one command).
# - md5 guards: run-daemon.sh and host186/daemon.py must be what this build was checked against (STAGED_*), else stop.
# - the Rust binary is bin/t2r-daemon-tcp (bin/t2r-daemon stays the UDP relay of t2r-relay.service)
# - same cwd (host186), same env, same files (live-matches.json re-adopt, bans.txt, team_rules.txt, version.txt, t2r.db); T2R_TOOLS_DIR = that
#   folder (Python reads its data files next to daemon.py, the Rust binary next to itself unless told); T2R_DB_WAL=1 = the database in WAL mode,
#   backups from a read snapshot (no request waits on a backup)
set -e
cd $HOME/t2r-live
S=staging/daemonrs
[ "$(md5sum run-daemon.sh | cut -d' ' -f1)" = "$(cat $S/run-daemon.md5)" ] || { echo "run-daemon.sh changed since staging, stop"; exit 1; }
[ "$(md5sum host186/daemon.py | cut -d' ' -f1)" = "$(cat $S/daemon-py.md5)" ] || { echo "host186/daemon.py changed since staging (port is checked against $(cat $S/daemon-py.md5)), stop"; exit 1; }
[ "$(md5sum $S/t2r-daemon-tcp | cut -d' ' -f1)" = "$(cat $S/t2r-daemon-tcp.md5)" ] || { echo "staged binary md5 wrong, stop"; exit 1; }
cp -p run-daemon.sh run-daemon.sh.bak-daemonrs
install -m 755 $S/t2r-daemon-tcp bin/t2r-daemon-tcp
grep -q '^  python3 -u daemon.py ' run-daemon.sh || { echo "daemon start line not found, stop"; exit 1; }
sed -i 's|^  python3 -u daemon.py |  T2R_DB_WAL=1 T2R_TOOLS_DIR=$PWD $HOME/t2r-live/bin/t2r-daemon-tcp serve |' run-daemon.sh
grep -n 't2r-daemon-tcp' run-daemon.sh
sudo systemctl restart t2r-daemon
sleep 20
P=$(for p in $(pgrep -u kellie -f "t2r-daemon-tcp serve"); do [ "$(readlink /proc/$p/cwd)" = "$HOME/t2r-live/host186" ] && echo $p; done | head -1)
echo "daemon pid ${P:-NONE}"
[ -n "$P" ] && { grep "open files" /proc/$P/limits; ps -o pid,etime,pcpu,rss,cmd -p $P; }
systemctl is-active t2r-relay
tail -n 400 $HOME/t2r-live/host186/daemon.log | grep -a "panick\|readopt\|re-adopt\|LISTEN" | tail -8 | cut -c1-200
