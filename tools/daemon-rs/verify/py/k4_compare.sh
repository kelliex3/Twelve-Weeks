#!/bin/bash
# k4_compare.sh FIRST LAST N : differential run of the K4 pure parts
set -u
a=$1; b=$2; n=$3
here=$(cd "$(dirname "$0")/../.." && pwd)
pydir=$here/py
tmp=${TMPDIR:-/tmp}/k4cmp; mkdir -p $tmp
bad=0
for s in $(seq $a $b); do
  export T2R_PY_DIR=$pydir T2R_TOOLS_DIR=$tmp/tools T2R_TEST_SEED=$s
  case $((s % 4)) in 1) export T2R_REGION_LOCK=0 ;; *) unset T2R_REGION_LOCK ;; esac
  case $((s % 3)) in 1) export T2R_REGIONS="fra=1.2.3.4:7001,nyc=5.6.7.8:7002@127.0.0.1,loop=127.0.0.1:7003,pub=209.25.141.16:15389,x=bad,nobody" ;; *) unset T2R_REGIONS ;; esac
  rm -rf $tmp/tools; rm -f $tmp/py.db $tmp/rs.db
  python3 $here/verify/py/k4_harness.py gen $s $n > $tmp/in.$s
  T2R_DB=$tmp/py.db python3 $here/verify/py/k4_harness.py run < $tmp/in.$s > $tmp/py.$s 2> $tmp/py.err.$s
  rm -rf $tmp/tools
  T2R_DB=$tmp/rs.db $here/t2r-daemon selftest k4 < $tmp/in.$s > $tmp/rs.$s 2> $tmp/rs.err.$s
  if cmp -s $tmp/py.$s $tmp/rs.$s; then echo "seed $s OK $(wc -l < $tmp/py.$s) lines"; rm -f $tmp/in.$s $tmp/py.$s $tmp/rs.$s $tmp/py.err.$s $tmp/rs.err.$s
  else echo "seed $s DIFF"; cmp $tmp/py.$s $tmp/rs.$s | head -2; bad=$((bad+1)); fi
done
echo "k4: $bad seeds differ"
