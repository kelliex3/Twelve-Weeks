#!/bin/bash
# k3_compare.sh FIRST LAST N : differential run of the K3 chunk (needs ./t2r-daemon, ./verify/py/, ./py = private copy of the deployed .py files)
set -u
a=$1; b=$2; n=$3
here=$(cd "$(dirname "$0")/../.." && pwd)
pydir=$here/py
tmp=/tmp/k3cmp; mkdir -p $tmp
bad=0
for s in $(seq $a $b); do
  export T2R_PY_DIR=$pydir T2R_TEST_SEED=$s T2R_TOOLS_DIR=$tmp/tools
  case $((s % 3)) in 0) export T2R_SP_MAPS="" ;; 1) export T2R_SP_MAPS=all ;; 2) export T2R_SP_MAPS=hardware,motel ;; esac
  case $(((s / 3) % 3)) in 0) export T2R_HOST_CAP=0 ;; 1) export T2R_HOST_CAP=1 ;; 2) export T2R_HOST_CAP=2 ;; esac
  case $((s % 4)) in 3) export T2R_REQUIRE_VERSION=1 ;; *) unset T2R_REQUIRE_VERSION ;; esac
  if [ $((s % 2)) = 1 ]; then export T2R_BOOSTER_SECS=3; else unset T2R_BOOSTER_SECS; fi
  case $((s % 5)) in 0) export T2R_VOTE_SECS=15 ;; 1) export T2R_VOTE_SECS=4 ;; *) unset T2R_VOTE_SECS ;; esac
  case $((s % 7)) in 0) export T2R_LOADING_SECS=2 ;; *) unset T2R_LOADING_SECS ;; esac
  mkdir -p $tmp/cfg; if [ $((s % 5)) = 2 ]; then export T2R_MAP_PACKS=1 T2R_MAP_PACKS_FILE=$tmp/cfg/packs.json; echo '{"packs":[{"id":"pk1","ver":1,"hash":"abcd1234","maps":[["Pack One","pk-one","to1-pk-one",12],["Pack Two","pk-two","to1-pk-two",9]]},{"id":"pk2","ver":2,"hash":"11112222","maps":[["Pack Three","pk-three","to1-pk-three",15]]}]}' > $tmp/cfg/packs.json; else unset T2R_MAP_PACKS T2R_MAP_PACKS_FILE; fi
  if [ $((s % 3)) = 1 ]; then export T2R_PING_MAX=12; else unset T2R_PING_MAX; fi
  rm -rf $tmp/tools; mkdir -p $tmp/tools; rm -f $tmp/py.db* $tmp/rs.db* $tmp/*.json $tmp/no_host.txt
  python3 $here/verify/py/k3_harness.py gen $s $n > $tmp/in.$s
  T2R_DB=$tmp/py.db python3 $here/verify/py/k3_harness.py run < $tmp/in.$s > $tmp/py.$s 2> $tmp/py.err.$s
  rm -f $tmp/*.json $tmp/no_host.txt
  T2R_DB=$tmp/rs.db $here/t2r-daemon selftest k3 < $tmp/in.$s > $tmp/rs.$s 2> $tmp/rs.err.$s
  if [ "$(wc -l < $tmp/py.$s)" != "$(wc -l < $tmp/in.$s)" ]; then echo "seed $s HARNESS FAILED ($(wc -l < $tmp/py.$s) of $(wc -l < $tmp/in.$s) lines)"; tail -3 $tmp/py.err.$s; bad=$((bad+1));
  elif cmp -s $tmp/py.$s $tmp/rs.$s; then echo "seed $s OK $(wc -l < $tmp/py.$s) lines"; [ -n "${K3_KEEP:-}" ] || rm -f $tmp/in.$s $tmp/py.$s $tmp/rs.$s $tmp/py.err.$s $tmp/rs.err.$s
  else echo "seed $s DIFF"; cmp $tmp/py.$s $tmp/rs.$s | head -2; bad=$((bad+1)); fi
done
echo "k3: $bad seeds differ"
