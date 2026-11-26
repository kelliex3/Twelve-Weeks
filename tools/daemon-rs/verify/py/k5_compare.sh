#!/bin/bash
# k5_compare.sh FIRST LAST N : differential run of the K5 chunk (needs ./t2r-daemon, ./verify/py/, ./py = private copy of the deployed .py files)
set -u
a=$1; b=$2; n=$3
here=$(cd "$(dirname "$0")/../.." && pwd)
pydir=$here/py
tmp=${TMPDIR:-/tmp}/k5cmp; mkdir -p $tmp
bad=0
for s in $(seq $a $b); do
  export T2R_PY_DIR=$pydir T2R_TEST_SEED=$s T2R_TOOLS_DIR=$tmp/tools T2R_PRIVATE=0 T2R_HOST_CAP=0
  case $((s % 4)) in 0) export T2R_STEAM_ONLY=0 ;; *) unset T2R_STEAM_ONLY ;; esac
  case $((s % 5)) in 1) export T2R_ANNOUNCE_CLOSE=1 ;; *) unset T2R_ANNOUNCE_CLOSE ;; esac
  case $((s % 6)) in 2) export T2R_PRESTART_HOSTLOST_REQUEUE=0 ;; *) unset T2R_PRESTART_HOSTLOST_REQUEUE ;; esac
  case $((s % 7)) in 3) export T2R_NO_PARTY_LOBBY=1 ;; *) unset T2R_NO_PARTY_LOBBY ;; esac
  case $((s % 3)) in 0) export T2R_JIP=0 ;; *) unset T2R_JIP ;; esac
  case $((s % 8)) in 5) export T2R_LEAVE_KEEP=60 ;; *) unset T2R_LEAVE_KEEP ;; esac
  rm -rf $tmp/tools; mkdir -p $tmp/tools; rm -f $tmp/py.db* $tmp/rs.db* $tmp/*.json $tmp/no_host.txt $tmp/mutes.txt $tmp/voicebans.txt $tmp/slurs.txt
  python3 $here/verify/py/k5_harness.py gen $s $n > $tmp/in.$s
  T2R_DB=$tmp/py.db python3 $here/verify/py/k5_harness.py run < $tmp/in.$s > $tmp/py.$s 2> $tmp/py.err.$s
  rm -f $tmp/*.json $tmp/no_host.txt $tmp/mutes.txt $tmp/voicebans.txt $tmp/slurs.txt
  T2R_DB=$tmp/rs.db $here/t2r-daemon selftest k5 < $tmp/in.$s > $tmp/rs.$s 2> $tmp/rs.err.$s
  if [ "$(wc -l < $tmp/py.$s)" != "$(wc -l < $tmp/in.$s)" ]; then echo "seed $s HARNESS FAILED ($(wc -l < $tmp/py.$s) of $(wc -l < $tmp/in.$s) lines)"; tail -3 $tmp/py.err.$s; bad=$((bad+1));
  elif cmp -s $tmp/py.$s $tmp/rs.$s; then echo "seed $s OK $(wc -l < $tmp/py.$s) lines"; [ -n "${K5_KEEP:-}" ] || rm -f $tmp/in.$s $tmp/py.$s $tmp/rs.$s $tmp/py.err.$s $tmp/rs.err.$s
  else echo "seed $s DIFF"; cmp $tmp/py.$s $tmp/rs.$s | head -2; bad=$((bad+1)); fi
done
echo "k5: $bad seeds differ"
