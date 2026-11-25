#!/bin/bash
# Differential run of the clan base modules on the test box:  clan_base_compare.sh FIRST_SEED LAST_SEED CALLS [PYDIR]
# needs ./t2r-daemon (musl build) next to ./verify/py/ ; PYDIR = a private COPY of the deployed .py files (the switch files are written there;
# default ./py). Both sides run with PYDIR as working directory.
set -u
a=$1; b=$2; n=$3
here=$(cd "$(dirname "$0")/../.." && pwd)
pydir=${4:-$here/py}
tmp=${TMPDIR:-/tmp}/cl_base_cmp
mkdir -p "$tmp"
bad=0
for s in $(seq "$a" "$b"); do
  export T2R_PY_DIR=$pydir T2R_TEST_SEED=$s
  python3 "$here/verify/py/clan_base_harness.py" gen "$s" "$n" > "$tmp/in.$s" || { echo "seed $s GEN FAILED"; bad=$((bad+1)); continue; }
  rm -rf "$tmp/py.bk" "$tmp/rs.bk"
  (cd "$pydir" && T2R_CL_DB="$tmp/py.db" T2R_CL_DIR="$tmp/py.bk" python3 "$here/verify/py/clan_base_harness.py" run < "$tmp/in.$s" > "$tmp/py.$s" 2> "$tmp/py.err.$s")
  (cd "$pydir" && T2R_CL_DB="$tmp/rs.db" T2R_CL_DIR="$tmp/rs.bk" "$here/t2r-daemon" selftest clan_base < "$tmp/in.$s" > "$tmp/rs.$s" 2> "$tmp/rs.err.$s")
  if [ -s "$tmp/py.$s" ] && cmp -s "$tmp/py.$s" "$tmp/rs.$s"; then
    echo "seed $s OK $(wc -l < "$tmp/py.$s") lines"
    rm -f "$tmp/in.$s" "$tmp/py.$s" "$tmp/rs.$s" "$tmp/py.err.$s" "$tmp/rs.err.$s"
  else
    echo "seed $s DIFF"
    cmp "$tmp/py.$s" "$tmp/rs.$s" | head -2
    bad=$((bad+1))
  fi
done
echo "clan_base: $bad seeds differ"
[ "$bad" = 0 ]
