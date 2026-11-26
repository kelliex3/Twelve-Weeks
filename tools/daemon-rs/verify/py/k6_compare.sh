#!/bin/bash
# k6_compare.sh FIRST LAST N : differential run of the K6 chunk (needs ./t2r-daemon, ./verify/py/, ./py = private copy of the deployed .py files)
set -u
a=$1; b=$2; n=$3
here=$(cd "$(dirname "$0")/../.." && pwd)
pydir=$here/py
tmp=/tmp/k6cmp; mkdir -p $tmp
bad=0
for s in $(seq $a $b); do
  export T2R_HOST_MIGRATION=1 T2R_HOST_REPICK=2 T2R_STATUS_FILE=0 T2R_PRIVATE=0 T2R_HOST_CAP=0 T2R_VERSION=k6test T2R_LOSS_HOST_S=30
  export T2R_PY_DIR=$pydir T2R_TEST_SEED=$s T2R_TOOLS_DIR=$tmp/tools PYTHONHASHSEED=0
  rm -rf $tmp/py $tmp/rs $tmp/tools; mkdir -p $tmp/py $tmp/rs $tmp/tools
  python3 $here/verify/py/k6_harness.py gen $s $n > $tmp/in.$s
  T2R_DB=$tmp/py/x.db python3 $here/verify/py/k6_harness.py run < $tmp/in.$s > $tmp/py.$s 2> $tmp/py.err.$s
  rm -rf $tmp/tools; mkdir -p $tmp/tools
  T2R_DB=$tmp/rs/x.db T2R_TEST_EPOCH=1 $here/t2r-daemon selftest k6 < $tmp/in.$s > $tmp/rs.$s 2> $tmp/rs.err.$s
  if [ "$(wc -l < $tmp/py.$s)" != "$(wc -l < $tmp/in.$s)" ]; then echo "seed $s HARNESS FAILED ($(wc -l < $tmp/py.$s) of $(wc -l < $tmp/in.$s) lines)"; tail -3 $tmp/py.err.$s; bad=$((bad+1));
  elif cmp -s $tmp/py.$s $tmp/rs.$s; then echo "seed $s OK $(wc -l < $tmp/py.$s) lines"; [ -n "${K6_KEEP:-}" ] || rm -f $tmp/in.$s $tmp/py.$s $tmp/rs.$s $tmp/py.err.$s $tmp/rs.err.$s
  else echo "seed $s DIFF"; cmp $tmp/py.$s $tmp/rs.$s | head -2; bad=$((bad+1)); fi
done
echo "k6: $bad seeds differ"
