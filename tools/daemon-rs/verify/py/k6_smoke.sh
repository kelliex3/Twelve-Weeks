#!/bin/bash
# k6_smoke.sh : `t2r-daemon serve` smoke test (test ports 7301 / 7311, never 7001): starts, answers admin /status + /live, the second daemon on the same
# port exits 3, SIGTERM stops the first with rc 0. Run from a folder with ./t2r-daemon and ./verify/bot/env.sh.
here=$(cd "$(dirname "$0")/../.." && pwd)
W=${K6_SMOKE_DIR:-/tmp/k6smoke}
rm -rf $W; mkdir -p $W/a $W/b
. $here/verify/bot/env.sh
export T2R_PORT=7301 T2R_ADMIN_PORT=7311 T2R_BIND_RETRY_S=2
cd $W/a
T2R_DB=$W/a/t2r.db T2R_LOG_PATH=$W/a/daemon.log $here/t2r-daemon serve > $W/a/stdout.txt 2>&1 &
PID=$!
for i in $(seq 1 50); do (exec 3<>/dev/tcp/127.0.0.1/7311) 2>/dev/null && break; sleep 0.2; done
echo "--- GET /status"
curl -s -i http://127.0.0.1:7311/status; echo
SECRET=$(cat $W/a/admin-secret.txt)
echo "--- GET /live (secret)"
curl -s -H "X-Admin-Secret: $SECRET" http://127.0.0.1:7311/live | head -c 300; echo
echo "--- GET /live (no secret)"
curl -s -i http://127.0.0.1:7311/live | head -3
echo "--- second daemon on the same ports"
cd $W/b
T2R_DB=$W/b/t2r.db T2R_LOG_PATH=$W/b/daemon.log timeout 30 $here/t2r-daemon serve > $W/b/stdout.txt 2>&1
echo "second rc=$?"
echo "--- SIGTERM"
kill -TERM $PID
wait $PID
echo "first rc=$?"
echo "--- log tail"
tail -n 12 $W/a/daemon.log
