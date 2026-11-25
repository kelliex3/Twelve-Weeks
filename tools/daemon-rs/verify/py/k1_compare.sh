#!/bin/bash
# k1_compare.sh FIRST LAST N : differential run of the K1 base chunk (needs ./t2r-daemon, ./verify/py/, ./py = private copy of the deployed .py files)
set -u
a=$1; b=$2; n=$3
here=$(cd "$(dirname "$0")/../.." && pwd)
pydir=$here/py
tmp=/tmp/k1cmp; mkdir -p $tmp
bad=0
for s in $(seq $a $b); do
  export T2R_PY_DIR=$pydir T2R_TEST_SEED=$s T2R_TOOLS_DIR=$tmp
  rm -f $tmp/py.db* $tmp/rs.db* $tmp/bans.txt; rm -rf $tmp/update-data
  python3 $here/verify/py/k1_harness.py gen $s $n > $tmp/in.$s
  T2R_DB=$tmp/py.db python3 $here/verify/py/k1_harness.py run < $tmp/in.$s > $tmp/py.$s 2> $tmp/py.err.$s
  rm -f $tmp/bans.txt; rm -rf $tmp/update-data
  T2R_DB=$tmp/rs.db $here/t2r-daemon selftest k1 < $tmp/in.$s > $tmp/rs.$s 2> $tmp/rs.err.$s
  if cmp -s $tmp/py.$s $tmp/rs.$s; then echo "seed $s OK $(wc -l < $tmp/py.$s) lines"; rm -f $tmp/in.$s $tmp/py.$s $tmp/rs.$s $tmp/py.err.$s $tmp/rs.err.$s
  else echo "seed $s DIFF"; cmp $tmp/py.$s $tmp/rs.$s | head -2; bad=$((bad+1)); fi
done
echo "k1: $bad seeds differ"
