#!/bin/bash
# run_all.sh <python|rust> <outdir> [scenario ...]
# Runs every scenario (or the listed ones) one after the other, each against its own fresh daemon, transcripts in <outdir>/<scenario>.txt.
# Env: T2R_TEST_SEED (default 1125), T2R_FAST=0 for the live-length match start (about 60 s per match instead of 25 s),
#      T2R_RS_BIN / T2R_RS_ARGS for the Rust daemon, T2R_PYDIR for the Python files, T2R_TEST_PORT / T2R_TEST_ADMIN_PORT.
KIND=$1; OUT=$2; shift 2
[ -z "$KIND" ] || [ -z "$OUT" ] && { echo "usage: run_all.sh <python|rust> <outdir> [scenario ...]"; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=${T2R_VERIFY_ROOT:-$HOME/rsverify/daemon/d}
BOT=${T2R_BOT:-$ROOT/bin/t2r-bot}
LIST="$@"
[ -z "$LIST" ] && LIST=$("$BOT" list | awk '{print $1}')
FAIL=0
for s in $LIST; do
  "$HERE/run_one.sh" "$KIND" "$s" "$OUT" "${T2R_TEST_SEED:-1125}" || { echo "FAILED: $s"; FAIL=1; }
done
exit $FAIL
