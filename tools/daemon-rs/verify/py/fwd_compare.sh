#!/bin/bash
# fwd_compare.sh FIRST LAST N : differential run of the T2RFWD first-line parser (needs ./t2r-daemon, ./verify/py/, ./py = the .py files to compare)
set -u
a=$1; b=$2; n=$3
here=$(cd "$(dirname "$0")/../.." && pwd)
pydir=$here/py
tmp=${TMPDIR:-/tmp}/fwdcmp; mkdir -p $tmp
bad=0
for s in $(seq $a $b); do
  export T2R_PY_DIR=$pydir T2R_FIRST_FRAME_S=1 T2R_TEST_SEED=$s T2R_DB=$tmp/py.db T2R_TOOLS_DIR=$tmp/tools
  python3 $here/verify/py/fwd_harness.py gen $s $n > $tmp/in.$s
  python3 $here/verify/py/fwd_harness.py run < $tmp/in.$s > $tmp/py.$s 2> $tmp/py.err.$s
  T2R_DB=$tmp/rs.db $here/t2r-daemon selftest fwd < $tmp/in.$s > $tmp/rs.$s 2> $tmp/rs.err.$s
  if cmp -s $tmp/py.$s $tmp/rs.$s; then echo "seed $s OK $(wc -l < $tmp/py.$s) lines"; rm -f $tmp/in.$s $tmp/py.$s $tmp/rs.$s $tmp/py.err.$s $tmp/rs.err.$s
  else echo "seed $s DIFF"; cmp $tmp/py.$s $tmp/rs.$s | head -2; bad=$((bad+1)); fi
done
echo "fwd: $bad seeds differ"
