#!/bin/bash
# GOLDEN test of the clan engine on the test box:  clan_golden_compare.sh FIRST_SEED LAST_SEED DAYS [CLANS] [PYDIR]
# needs ./t2r-daemon (musl build) next to ./verify/py/ ; PYDIR = a private COPY of the deployed .py files (default ./py). Both sides run in PYDIR.
# A seed that differs is re-run with T2R_GOLD_FULL=1 (the rows of every table after every day, the full text of every in-game page) and the first difference is shown.
set -u
a=$1; b=$2; days=$3
clans=${4:-8}
here=$(cd "$(dirname "$0")/../.." && pwd)
pydir=${5:-$here/py}
tmp=${TMPDIR:-/tmp}/cl_golden_cmp
mkdir -p "$tmp"
bad=0
H="$here/verify/py/clan_golden_harness.py"
for s in $(seq "$a" "$b"); do
  export T2R_PY_DIR=$pydir T2R_TEST_SEED=$s
  (cd "$pydir" && python3 "$H" gen "$s" "$days" "$clans" > "$tmp/in.$s") || { echo "seed $s GEN FAILED"; bad=$((bad+1)); continue; }
  rm -rf "$tmp/gd_py" "$tmp/gd_rs"
  (cd "$pydir" && T2R_CL_DB="$tmp/py.db" T2R_GOLD_DIR="$tmp/gd_py" python3 "$H" run < "$tmp/in.$s" > "$tmp/py.$s" 2> "$tmp/py.err.$s")
  (cd "$pydir" && T2R_CL_DB="$tmp/rs.db" T2R_GOLD_DIR="$tmp/gd_rs" "$here/t2r-daemon" selftest clan_golden < "$tmp/in.$s" > "$tmp/rs.$s" 2> "$tmp/rs.err.$s")
  if [ -s "$tmp/py.$s" ] && cmp -s "$tmp/py.$s" "$tmp/rs.$s"; then
    echo "seed $s OK $(wc -l < "$tmp/in.$s") events $(wc -c < "$tmp/py.$s") bytes"
    rm -f "$tmp/in.$s" "$tmp/py.$s" "$tmp/rs.$s" "$tmp/py.err.$s" "$tmp/rs.err.$s"
  else
    echo "seed $s DIFF"
    rm -rf "$tmp/gd_py" "$tmp/gd_rs"
    (cd "$pydir" && T2R_GOLD_FULL=1 T2R_TRACE=1 T2R_CL_DB="$tmp/py.db" T2R_GOLD_DIR="$tmp/gd_py" python3 "$H" run < "$tmp/in.$s" > "$tmp/py.$s" 2> "$tmp/py.err.$s")
    (cd "$pydir" && T2R_GOLD_FULL=1 T2R_TRACE=1 T2R_CL_DB="$tmp/rs.db" T2R_GOLD_DIR="$tmp/gd_rs" "$here/t2r-daemon" selftest clan_golden < "$tmp/in.$s" > "$tmp/rs.$s" 2> "$tmp/rs.err.$s")
    python3 "$here/verify/py/clan_golden_diff.py" "$tmp/in.$s" "$tmp/py.$s" "$tmp/rs.$s" 2>&1 | head -30
    head -c 600 "$tmp/py.err.$s" "$tmp/rs.err.$s" 2>/dev/null | head -20
    bad=$((bad+1))
  fi
done
echo "clan_golden: $bad seeds differ"
[ "$bad" = 0 ]
