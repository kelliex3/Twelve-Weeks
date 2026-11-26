#!/bin/bash
# Shadow load (sfo3 only): one daemon (rust = t2r-daemon serve, python = daemon.py) on a COPY of the live database with the live environment,
# t2r-bot load against it, the daemon's CPU / RSS / threads / fds sampled every 10 s.
# usage: bash verify/shadow/shadow.sh <rust|python> SECS SRC_DB [GROUPS] [IDLE]
# needs: ./t2r-daemon (rust), ./py (python files), ./bin/t2r-bot; writes shadow-<kind>/{samples.csv,load.out,daemon.log,summary.txt}
set -u
KIND=$1; SECS=$2; SRC=$3; G=${4:-8}; IDLE=${5:-60}
HERE=$(cd "$(dirname "$0")/../.." && pwd)
W=$PWD/shadow-$KIND
rm -rf "$W"; mkdir -p "$W"
cp "$SRC" "$W/t2r.db"
cp -r "$HERE/py" "$W/run"; rm -f "$W/run/team_rules.txt" "$W/run/live-matches.json"
. "$HERE/verify/bot/env.sh"
unset T2R_TEST_SEED T2R_TEST_SEED_STREAMS T2R_TEST_EPOCH T2R_LB_CACHE_S T2R_SEARCH_CACHE_S T2R_PRE_COUNTDOWN T2R_VOTE_SECS T2R_COUNTDOWN
export T2R_PRE_COUNTDOWN=2 T2R_VOTE_SECS=10 T2R_COUNTDOWN=4     # shorter match start, more matches per hour
export T2R_DB=$W/t2r.db T2R_LOG_PATH=$W/daemon.log T2R_PORT=7401 T2R_ADMIN_PORT=7411 T2R_TOOLS_DIR=$W/run T2R_PING_KICK_MS=0 T2R_DB_BACKUP_S=1200 T2R_NEW_TOKENS_PER_10MIN=100000   # backups every 20 min; all bots come from one address
[ "$KIND" = rust ] && export T2R_DB_WAL=1
cd "$W/run"
if [ "$KIND" = python ]; then python3 -u daemon.py > "$W/stdout.txt" 2>&1 & else "$HERE/t2r-daemon" serve > "$W/stdout.txt" 2>&1 & fi
PID=$!
for i in $(seq 1 100); do (exec 3<>/dev/tcp/127.0.0.1/7401) 2>/dev/null && break; sleep 0.3; done
cd "$W"
echo "t,cpu_s,rss_kb,threads,fds" > samples.csv
(
  T0=$(date +%s); CLK=$(getconf CLK_TCK)
  while kill -0 $PID 2>/dev/null; do
    read -r ut st <<< "$(awk '{print $14, $15}' /proc/$PID/stat)"
    rss=$(awk '/VmRSS/{print $2}' /proc/$PID/status); th=$(awk '/Threads/{print $2}' /proc/$PID/status); fd=$(ls /proc/$PID/fd | wc -l)
    echo "$(( $(date +%s) - T0 )),$(( (ut + st) / CLK )),$rss,$th,$fd" >> samples.csv
    sleep 10
  done
) &
SP=$!
"$HERE/bin/t2r-bot" load --port 7401 --admin-port 7411 --groups $G --size 8 --idle $IDLE --secs $SECS --version "$T2R_VERSION" > load.out 2>&1
sleep 5
alive=no; kill -0 $PID 2>/dev/null && alive=yes
kill $PID 2>/dev/null; wait $PID 2>/dev/null
kill $SP 2>/dev/null
{
  echo "kind=$KIND secs=$SECS groups=$G idle=$IDLE daemon_alive_at_end=$alive"
  tail -2 load.out
  python3 - "$W/samples.csv" <<'PY'
import csv, sys
r = list(csv.DictReader(open(sys.argv[1])))
if len(r) > 2:
    a, b = r[1], r[-1]
    dt = int(b["t"]) - int(a["t"])
    cpu = (int(b["cpu_s"]) - int(a["cpu_s"])) / max(1, dt) * 100
    rss = [int(x["rss_kb"]) for x in r]
    print("avg CPU %.1f %% of one core over %d s; RSS start %d MB, max %d MB, end %d MB; threads max %d; fds max %d" % (
        cpu, dt, rss[1] // 1024, max(rss) // 1024, rss[-1] // 1024, max(int(x["threads"]) for x in r), max(int(x["fds"]) for x in r)))
PY
  echo "log: $(grep -ac . daemon.log) lines, Traceback $(grep -ac Traceback daemon.log), panic $(grep -aic panic daemon.log stdout.txt | paste -sd+), HANDLER ERR $(grep -ac 'HANDLER ERR' daemon.log)"
} > summary.txt
cat summary.txt
