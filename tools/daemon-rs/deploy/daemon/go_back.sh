#!/bin/bash
# Rollback of go.sh: python3 daemon.py again (run-daemon.sh.bak-daemonrs), restart. The database stays in WAL mode since go.sh sets
# T2R_DB_WAL=1: it is switched back to journal_mode=DELETE while no daemon runs (the Python restore path keeps no -wal file).
set -e
cd $HOME/t2r-live
[ -f run-daemon.sh.bak-daemonrs ] || { echo "no run-daemon.sh.bak-daemonrs, stop"; exit 1; }
cp -p run-daemon.sh.bak-daemonrs run-daemon.sh
sudo systemctl stop t2r-daemon
python3 -c "import sqlite3,sys; c=sqlite3.connect(sys.argv[1], timeout=30); print('journal', c.execute('PRAGMA journal_mode=DELETE').fetchone()[0])" data/t2r.db
sudo systemctl start t2r-daemon
sleep 20
for p in $(pgrep -u kellie -f "python3 -u daemon.py"); do [ "$(readlink /proc/$p/cwd)" = "$HOME/t2r-live/host186" ] && ps -o pid,etime,cmd -p $p; done
tail -n 300 $HOME/t2r-live/host186/daemon.log | grep -a "Traceback\|re-adopt" | tail -5 | cut -c1-200
