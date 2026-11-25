#!/bin/bash
# k4_compare.sh FIRST LAST N : differential run of the K4 pure parts
set -u
a=$1; b=$2; n=$3
here=$HOME/rsverify/daemon/k4
pydir=$HOME/rsverify/daemon/py
tmp=/tmp/k4cmp; mkdir -p $tmp
bad=0
for s in $(seq $a $b); do
  export T2R_PY_DIR=$pydir T2R_TOOLS_DIR=$tmp/tools T2R_TEST_SEED=$s
  rm -rf $tmp/tools; rm -f $tmp/py.db $tmp/rs.db
  python3 $here/verify/py/k4_harness.py gen $s $n > $tmp/in.$s
  T2R_DB=$tmp/py.db python3 $here/verify/py/k4_harness.py run < $tmp/in.$s > $tmp/py.$s 2> $tmp/py.err.$s
  rm -rf $tmp/tools
  T2R_DB=$tmp/rs.db $here/t2r-daemon selftest k4 < $tmp/in.$s > $tmp/rs.$s 2> $tmp/rs.err.$s
  if cmp -s $tmp/py.$s $tmp/rs.$s; then echo "seed $s OK $(wc -l < $tmp/py.$s) lines"; rm -f $tmp/in.$s $tmp/py.$s $tmp/rs.$s $tmp/py.err.$s $tmp/rs.err.$s
  else echo "seed $s DIFF"; cmp $tmp/py.$s $tmp/rs.$s | head -2; bad=$((bad+1)); fi
done
echo "k4: $bad seeds differ"
