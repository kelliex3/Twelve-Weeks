#!/bin/bash
# Differential run of one module (lobbybook requeue modeq gunlimit interrogation map_packs) on the test box:
#   c_compare.sh MODULE FIRST_SEED LAST_SEED CALLS [PYDIR]
# needs ./t2r-daemon (musl build) and ./verify/py/*.py next to this script's parent; PYDIR = the deployed .py files
# (default ~/rsverify/daemon/py; with a modes.json / interrogation_lockboxes.json inside it the Rust side reads them via T2R_TOOLS_DIR).
set -u
mod=$1; a=$2; b=$3; n=$4
pydir=${5:-$HOME/rsverify/daemon/py}
here=$(cd "$(dirname "$0")/../.." && pwd)
tmp=${TMPDIR:-/tmp}/c_cmp_$mod
mkdir -p "$tmp"
bad=0
for s in $(seq "$a" "$b"); do
  export T2R_PY_DIR=$pydir T2R_TOOLS_DIR=$pydir T2R_TEST_SEED=$s
  python3 "$here/verify/py/${mod}_harness.py" gen "$s" "$n" > "$tmp/in.$s"
  python3 "$here/verify/py/${mod}_harness.py" run < "$tmp/in.$s" > "$tmp/py.$s" 2> "$tmp/py.err.$s"
  "$here/t2r-daemon" selftest "$mod" < "$tmp/in.$s" > "$tmp/rs.$s" 2> "$tmp/rs.err.$s"
  if cmp -s "$tmp/py.$s" "$tmp/rs.$s"; then
    echo "seed $s OK $(wc -l < "$tmp/py.$s") lines"
    rm -f "$tmp/in.$s" "$tmp/py.$s" "$tmp/rs.$s" "$tmp/py.err.$s" "$tmp/rs.err.$s"
  else
    echo "seed $s DIFF"
    cmp "$tmp/py.$s" "$tmp/rs.$s" | head -2
    bad=$((bad+1))
  fi
done
echo "$mod: $bad seeds differ"
[ "$bad" = 0 ]
