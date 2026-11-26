#!/bin/bash
# run_one.sh <python|rust> <scenario> <outdir> [seed]
# Starts ONE daemon (fresh empty db, pinned seed, the live environment of env.sh) in its own work dir, runs one scenario against it,
# and writes <outdir>/<scenario>.txt = script + streams (frames, admin JSON, snapshots) + db dump + normalised log.
# Python: seedhook.py + the deployed daemon.py from T2R_PYDIR (a COPY of the live files). Rust: T2R_RS_BIN with T2R_RS_ARGS (default "serve").
KIND=$1; SCN=$2; OUT=$3; SEED=${4:-1125}
HERE=$(cd "$(dirname "$0")" && pwd)
VERIFY=$(cd "$HERE/.." && pwd)
ROOT=${T2R_VERIFY_ROOT:-$HOME/rsverify/daemon/d}
BOT=${T2R_BOT:-$ROOT/bin/t2r-bot}
PYDIR=${T2R_PYDIR:-$ROOT/pyrun}
PORT=${T2R_TEST_PORT:-7301}
ADMIN=${T2R_TEST_ADMIN_PORT:-7311}
W=$ROOT/work/$KIND-$SCN
[ -z "$KIND" ] || [ -z "$SCN" ] || [ -z "$OUT" ] && { echo "usage: run_one.sh <python|rust> <scenario> <outdir> [seed]"; exit 2; }
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
rm -rf "$W"; mkdir -p "$W"
export T2R_TEST_SEED=$SEED
. "$HERE/env.sh"
export T2R_DB=$W/t2r.db T2R_LOG_PATH=$W/daemon.log T2R_PORT=$PORT T2R_ADMIN_PORT=$ADMIN
if ss -ltn 2>/dev/null | grep -q ":$PORT "; then echo "port $PORT is busy (another daemon of this harness?)"; exit 3; fi
rm -f "$PYDIR/team_rules.txt"          # POST /teams writes team_rules.txt next to daemon.py: start every run without rules
cd "$W"
if [ "$KIND" = python ]; then
  python3 "$VERIFY/py/seedhook.py" "$PYDIR/daemon.py" > "$W/stdout.txt" 2>&1 &
else
  "${T2R_RS_BIN:-$ROOT/bin/t2r-daemon}" ${T2R_RS_ARGS-serve} > "$W/stdout.txt" 2>&1 &
fi
PID=$!
up=0
for i in $(seq 1 150); do
  if (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && (exec 3<>/dev/tcp/127.0.0.1/$ADMIN) 2>/dev/null; then up=1; break; fi
  kill -0 $PID 2>/dev/null || break
  sleep 0.2
done
if [ $up != 1 ]; then echo "daemon did not come up ($KIND $SCN)"; tail -5 "$W/stdout.txt"; kill -9 $PID 2>/dev/null; exit 4; fi
sleep 0.5
timeout ${T2R_SCN_TIMEOUT:-900} "$BOT" run "$SCN" --port $PORT --admin-port $ADMIN --workdir "$W" --version "$T2R_VERSION" --out "$W/frames.txt" > "$W/bot.out" 2>&1
RC=$?
sleep 2.5
{
  cat "$W/frames.txt" 2>/dev/null
  echo "##### DB"
  python3 -I "$VERIFY/py/dbdump.py" "$W/t2r.db"
  echo "##### LOG"
  "$BOT" normlog "$W/daemon.log" "$W" "$PYDIR" "$ROOT"
} > "$OUT/$SCN.txt"
kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null
rm -f "$PYDIR/team_rules.txt"
echo "$KIND $SCN rc=$RC -> $OUT/$SCN.txt ($(wc -l < "$OUT/$SCN.txt") lines)"
exit $RC
