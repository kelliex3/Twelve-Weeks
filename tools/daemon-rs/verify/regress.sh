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
for m in lobbybook requeue modeq gunlimit interrogation map_packs; do run $m bash verify/py/c_compare.sh $m 1 $S $N "$P"; done
for m in hostmig hostcap; do run $m sh verify/hostmig_compare.sh $m "$P" ./t2r-daemon $N $(seq 1 $S); done
for m in social small; do T2R_PY_DIR=$P run $m sh verify/run_daemon_verify.sh $m $S $N; done
run clan_base bash verify/py/clan_base_compare.sh 1 $S $N "$P"
run clan_story bash verify/py/clan_story_compare.sh 1 $S $N "$P"
run clan_chronicle bash verify/py/clan_chronicle_compare.sh 1 $S $N "$P"
run clan_saga bash verify/py/clan_saga_compare.sh 1 $S $N "$P"
run k1 bash verify/py/k1_compare.sh 1 $S $N
mkdir -p ~/rsverify/daemon/k4 && rm -rf ~/rsverify/daemon/k4/verify && cp -r verify ~/rsverify/daemon/k4/ && cp t2r-daemon ~/rsverify/daemon/k4/t2r-daemon.new && mv ~/rsverify/daemon/k4/t2r-daemon.new ~/rsverify/daemon/k4/t2r-daemon
run k4 bash verify/py/k4_compare.sh 1 $S $N
run k4mm bash verify/py/k4mm_compare.sh 1 $S $N
rm -rf "$TMPDIR"
[ $fail = 0 ] && echo "REGRESS OK" || echo "REGRESS FAIL $fail"
