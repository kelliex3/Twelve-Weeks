#!/bin/bash
# Full regression of every differential test (run from a folder with ./t2r-daemon, ./verify/, ./py = private copy of the deployed .py files).
# regress.sh [SEEDS] [CALLS]   prints one summary line per module, "REGRESS OK" / "REGRESS FAIL n" at the end.
S=${1:-4}; N=${2:-1000}
cd "$(dirname "$0")/.."
P=$(pwd)/py
cp -f t2r-daemon verify/py/t2r-daemon 2>/dev/null
export TMPDIR=/tmp/regress_$$; mkdir -p "$TMPDIR"
fail=0
run() { local name=$1; shift; local out; out=$("$@" 2>&1); local rc=$?
  if [ $rc -ne 0 ] || echo "$out" | grep -qE 'DIFF|differ: [1-9]|[1-9][0-9]* seeds differ|FAIL'; then echo "FAIL $name"; echo "$out" | grep -vE ' OK |identical' | head -5; fail=$((fail+1)); else echo "ok   $name"; fi; }
for m in lobbybook requeue modeq gunlimit interrogation map_packs slurs voiceroom wslink; do run $m bash verify/py/c_compare.sh $m 1 $S $N "$P"; done
for m in hostmig hostcap; do run $m sh verify/hostmig_compare.sh $m "$P" ./t2r-daemon $N $(seq 1 $S); done
for m in social small; do T2R_PY_DIR=$P run $m sh verify/run_daemon_verify.sh $m $S $N; done
run clan_base bash verify/py/clan_base_compare.sh 1 $S $N "$P"
run clan_story bash verify/py/clan_story_compare.sh 1 $S $N "$P"
run clan_chronicle bash verify/py/clan_chronicle_compare.sh 1 $S $N "$P"
run clan_saga bash verify/py/clan_saga_compare.sh 1 $S $N "$P"
run clan_alife bash verify/py/clan_alife_compare.sh 1 $S $N "$P"
T2R_HARNESS=clan_journal_harness T2R_MOD=clan_journal run clan_journal bash verify/py/clan_alife_compare.sh 11 $((10+S)) $N "$P"
run clan_cl6 bash verify/py/clan_cl6_compare.sh 1 $S $N "$P"
T2R_REAL_STATUS=1 run clan_chronicle_real bash verify/py/clan_chronicle_compare.sh 1 $S $N "$P"
run wslink_loop bash -c "python3 verify/py/wslink_loop_test.py py $P > $TMPDIR/wl.py && python3 verify/py/wslink_loop_test.py rs ./t2r-daemon > $TMPDIR/wl.rs && cmp $TMPDIR/wl.py $TMPDIR/wl.rs || echo FAIL"
run clan_golden bash verify/py/clan_golden_compare.sh 1 1 3 4 "$P"
run k1 bash verify/py/k1_compare.sh 1 $S $N
run fwd bash verify/py/fwd_compare.sh 1 $S 60
T2R_K2_DOUBLES=0 run k2 bash verify/py/k2_compare.sh 1 $S $N
run k3 bash verify/py/k3_compare.sh 1 $S $N
run k6 bash verify/py/k6_compare.sh 1 $S $N
run k5 bash verify/py/k5_compare.sh 1 $S $N
run k4 bash verify/py/k4_compare.sh 1 $S $N
run k4mm bash verify/py/k4mm_compare.sh 1 $S $N
rm -rf "$TMPDIR"
[ $fail = 0 ] && echo "REGRESS OK" || echo "REGRESS FAIL $fail"
