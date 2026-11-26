#!/bin/bash
# Rollback of go.sh: python3 -u daemon.py again (run-daemon.sh.bak-daemonrs = the Python run line), restart. go.sh set T2R_DB_WAL=1, so the
# database is switched back to journal_mode=DELETE while no daemon runs (the Python restore path keeps no -wal file).
set -e
cd $HOME/t2r-live
[ -f run-daemon.sh.bak-daemonrs ] || { echo "no run-daemon.sh.bak-daemonrs, stop"; exit 1; }
grep -q '^  python3 -u daemon.py ' run-daemon.sh.bak-daemonrs || { echo "run-daemon.sh.bak-daemonrs has no Python run line, stop"; exit 1; }
cp -p run-daemon.sh.bak-daemonrs run-daemon.sh
sudo systemctl stop t2r-daemon
python3 -c "import sqlite3,sys; c=sqlite3.connect(sys.argv[1], timeout=30); print('journal', c.execute('PRAGMA journal_mode=DELETE').fetchone()[0])" data/t2r.db
sudo systemctl start t2r-daemon
sleep 25
D=$(cat host-dir)
for p in $(pgrep -u kellie -f "python3 -u daemon.py"); do [ "$(readlink /proc/$p/cwd)" = "$HOME/t2r-live/$D" ] && ps -o pid,etime,pcpu,rss,cmd -p $p; done
systemctl is-active t2r-daemon t2r-relay
tail -n 400 $D/daemon.log | grep -a "Traceback\|re-adopt\|LIVE-STATE" | tail -5 | cut -c1-200
