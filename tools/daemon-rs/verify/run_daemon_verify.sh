#!/bin/sh
# Differential run of one module: run_daemon_verify.sh <module> <seeds> [calls]   (module: social | small)
# Needs, in the current folder: t2r-daemon (the musl binary), py/ (the deployed Python files), and verify/py/*.
M=${1:-social}; SEEDS=${2:-10}; CALLS=${3:-2000}
export T2R_PY_DIR=${T2R_PY_DIR:-$HOME/rsverify/daemon/py}
export T2R_VERIFY_DIR=${T2R_VERIFY_DIR:-${TMPDIR:-/tmp}/rsv_$M}
mkdir -p "$T2R_VERIFY_DIR"
fail=0
s=1
while [ $s -le $SEEDS ]; do
  export T2R_TEST_SEED=$s
  python3 verify/py/gen_$M.py $s $CALLS > "$T2R_VERIFY_DIR/script.$s"
  if [ "$M" = social ]; then
    python3 verify/py/social_harness.py < "$T2R_VERIFY_DIR/script.$s" > "$T2R_VERIFY_DIR/py.$s"
    ./t2r-daemon selftest social < "$T2R_VERIFY_DIR/script.$s" > "$T2R_VERIFY_DIR/rs.$s"
    if cmp -s "$T2R_VERIFY_DIR/py.$s" "$T2R_VERIFY_DIR/rs.$s"; then echo "seed $s: identical ($(wc -l < "$T2R_VERIFY_DIR/py.$s") lines)"; else echo "seed $s: DIFFERENT"; fail=1; fi
  else
    ok=1
    for mod in names notices roles rankbadge; do
      grep "\"m\":\"$mod\"" "$T2R_VERIFY_DIR/script.$s" > "$T2R_VERIFY_DIR/script.$s.$mod"
      ./t2r-daemon selftest $mod < "$T2R_VERIFY_DIR/script.$s.$mod" > "$T2R_VERIFY_DIR/rs.$s.$mod"
      python3 verify/py/small_harness.py $mod < "$T2R_VERIFY_DIR/script.$s.$mod" > "$T2R_VERIFY_DIR/py.$s.$mod"
      if ! cmp -s "$T2R_VERIFY_DIR/py.$s.$mod" "$T2R_VERIFY_DIR/rs.$s.$mod"; then echo "seed $s $mod: DIFFERENT"; ok=0; fail=1; fi
    done
    [ $ok = 1 ] && echo "seed $s: identical"
  fi
  s=$((s+1))
done
exit $fail
