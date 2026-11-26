#!/bin/bash
# k2_compare.sh FIRST LAST N : differential run of the K2 clan glue (needs ./t2r-daemon, ./verify/py/, ./py = private copy of the deployed .py files)
# (both sides run with the python dir as working directory, each with its own DB folder: the clan reset backups live next to the DB)
set -u
a=$1; b=$2; n=$3
here=$(cd "$(dirname "$0")/../.." && pwd)
pydir=$here/py
tmp=${TMPDIR:-/tmp}/k2cmp; mkdir -p $tmp
bad=0
for s in $(seq $a $b); do
  export T2R_PY_DIR=$pydir T2R_TEST_SEED=$s T2R_TOOLS_DIR=$tmp/tools
  if [ $((s % 4)) = 0 ]; then export T2R_CLAN_NO_PENALTY=1; else unset T2R_CLAN_NO_PENALTY; fi   # (a server without clan penalties: every 4th seed)
  rm -rf $tmp/py $tmp/rs; mkdir -p $tmp/py $tmp/rs $tmp/tools
  python3 $here/verify/py/k2_harness.py gen $s $n > $tmp/in.$s
  (cd $pydir && T2R_DB=$tmp/py/t2r.db python3 $here/verify/py/k2_harness.py run < $tmp/in.$s > $tmp/py.$s 2> $tmp/py.err.$s)
  (cd $pydir && T2R_DB=$tmp/rs/t2r.db $here/t2r-daemon selftest k2 < $tmp/in.$s > $tmp/rs.$s 2> $tmp/rs.err.$s)
  if cmp -s $tmp/py.$s $tmp/rs.$s; then echo "seed $s OK $(wc -l < $tmp/py.$s) lines"; [ -n "${K2_KEEP:-}" ] || rm -f $tmp/in.$s $tmp/py.$s $tmp/rs.$s $tmp/py.err.$s $tmp/rs.err.$s
  else echo "seed $s DIFF"; cmp $tmp/py.$s $tmp/rs.$s | head -2; bad=$((bad+1)); fi
done
echo "k2: $bad seeds differ"
