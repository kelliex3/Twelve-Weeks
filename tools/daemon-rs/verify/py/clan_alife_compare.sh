#!/bin/bash
# Differential run of clan_story on the test box:  clan_story_compare.sh FIRST_SEED LAST_SEED CALLS [PYDIR]
# needs ./t2r-daemon (musl build) and ./clan_alife_harness.py next to this script; PYDIR = the deployed .py files (default ~/rsverify/daemon/py).
# Both sides run with the python dir as working directory and write a fresh sqlite file.
set -u
a=$1; b=$2; n=$3
pydir=${5:-${4:-$HOME/rsverify/daemon/py}}
here=$(cd "$(dirname "$0")" && pwd)
bin=${T2R_BIN:-$here/t2r-daemon}
tmp=${TMPDIR:-/tmp}/al_cmp
mkdir -p "$tmp"
bad=0
for s in $(seq "$a" "$b"); do
  export T2R_PY_DIR=$pydir
  python3 "$here/${T2R_HARNESS:-clan_alife_harness}.py" gen "$s" "$n" > "$tmp/in.$s"
  (cd "$pydir" && T2R_CL_DB=$tmp/py.$s.db python3 "$here/${T2R_HARNESS:-clan_alife_harness}.py" run < "$tmp/in.$s" > "$tmp/py.$s" 2> "$tmp/py.err.$s")
  (cd "$pydir" && T2R_CL_DB=$tmp/rs.$s.db "$bin" selftest ${T2R_MOD:-clan_alife} < "$tmp/in.$s" > "$tmp/rs.$s" 2> "$tmp/rs.err.$s")
  if cmp -s "$tmp/py.$s" "$tmp/rs.$s"; then
    echo "seed $s OK $(wc -l < "$tmp/py.$s") lines"
    rm -f "$tmp/in.$s" "$tmp/py.$s" "$tmp/rs.$s" "$tmp/py.err.$s" "$tmp/rs.err.$s" "$tmp/py.$s.db" "$tmp/rs.$s.db"
  else
    echo "seed $s DIFF"
    cmp "$tmp/py.$s" "$tmp/rs.$s" | head -2
    bad=$((bad+1))
  fi
done
echo "clan_alife: $bad seeds differ"
[ "$bad" = 0 ]
