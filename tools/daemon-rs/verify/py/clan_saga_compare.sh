#!/bin/bash
# Differential run of clan_saga + clan_saga_pl + clan_loot on the test box:  clan_saga_compare.sh FIRST_SEED LAST_SEED CALLS [PYDIR]
# needs ./t2r-daemon (musl build) next to ./verify/py/ ; PYDIR = a private COPY of the deployed .py files (default ./py). Both sides run in PYDIR.
set -u
a=$1; b=$2; n=$3
here=$(cd "$(dirname "$0")/../.." && pwd)
pydir=${4:-$here/py}
tmp=${TMPDIR:-/tmp}/cl_saga_cmp
mkdir -p "$tmp"
bad=0
for s in $(seq "$a" "$b"); do
  export T2R_PY_DIR=$pydir
  (cd "$pydir" && python3 "$here/verify/py/clan_saga_harness.py" gen "$s" "$n" > "$tmp/in.$s") || { echo "seed $s GEN FAILED"; bad=$((bad+1)); continue; }
  (cd "$pydir" && T2R_CL_DB="$tmp/py.db" python3 "$here/verify/py/clan_saga_harness.py" run < "$tmp/in.$s" > "$tmp/py.$s" 2> "$tmp/py.err.$s")
  (cd "$pydir" && T2R_CL_DB="$tmp/rs.db" "$here/t2r-daemon" selftest clan_saga < "$tmp/in.$s" > "$tmp/rs.$s" 2> "$tmp/rs.err.$s")
  dump='import sqlite3,sys;print(sqlite3.connect(sys.argv[1]).execute("select token_id,state,updated from clan_story order by token_id").fetchall())'
  python3 -c "$dump" "$tmp/py.db" > "$tmp/py.db.txt" 2>&1; python3 -c "$dump" "$tmp/rs.db" > "$tmp/rs.db.txt" 2>&1
  if [ -s "$tmp/py.$s" ] && cmp -s "$tmp/py.$s" "$tmp/rs.$s" && cmp -s "$tmp/py.db.txt" "$tmp/rs.db.txt"; then
    echo "seed $s OK $(wc -l < "$tmp/py.$s") lines"
    rm -f "$tmp/in.$s" "$tmp/py.$s" "$tmp/rs.$s" "$tmp/py.err.$s" "$tmp/rs.err.$s"
  else
    echo "seed $s DIFF"
    cmp "$tmp/py.$s" "$tmp/rs.$s" | head -2
    bad=$((bad+1))
  fi
done
echo "clan_saga: $bad seeds differ"
[ "$bad" = 0 ]
